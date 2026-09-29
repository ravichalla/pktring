// SPDX-License-Identifier: GPL-2.0
/*
 * pktring - a packet-oriented ring buffer character device.
 *
 * Models a software TX/RX descriptor ring like the ones a NIC driver uses:
 *   - one write() enqueues exactly one packet (message boundaries preserved)
 *   - one read() dequeues exactly one packet
 *   - ring full  -> writers block, or get -EAGAIN with O_NONBLOCK
 *   - ring empty -> readers block, or get -EAGAIN with O_NONBLOCK
 *   - poll()/select() supported; ioctl exposes counters and reset
 *
 * Error contract (verified by pktring_test.c):
 *   write: len==0 -> -EINVAL, len>MAX -> -EMSGSIZE, bad user ptr -> -EFAULT,
 *          full+O_NONBLOCK -> -EAGAIN. Failed writes never enqueue.
 *   read : buffer smaller than next packet -> -EMSGSIZE (packet is NOT consumed),
 *          bad user ptr -> -EFAULT (packet is NOT consumed),
 *          empty+O_NONBLOCK -> -EAGAIN.
 *   ioctl: unknown command -> -ENOTTY.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "pktring.h"

static unsigned int nslots = PKTRING_DEF_SLOTS;
module_param(nslots, uint, 0444);
MODULE_PARM_DESC(nslots, "Number of ring slots (1..4096, default 64)");

struct pkt_slot {
	u32 len;
	u8  data[PKTRING_MAX_PKT];
};

static struct {
	struct mutex          lock;
	wait_queue_head_t     rd_wq;   /* readers wait for data */
	wait_queue_head_t     wr_wq;   /* writers wait for space */
	struct pkt_slot      *slots;
	u32                   nslots;
	u32                   head;    /* next slot to write */
	u32                   tail;    /* next slot to read */
	u32                   count;   /* queued packets */
	struct pktring_stats  st;
} ring;

static ssize_t pktring_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	struct pkt_slot *s;

	if (mutex_lock_interruptible(&ring.lock))
		return -ERESTARTSYS;

	if (len == 0 || len > PKTRING_MAX_PKT) {
		ring.st.bad_write++;
		mutex_unlock(&ring.lock);
		return len == 0 ? -EINVAL : -EMSGSIZE;
	}

	while (ring.count == ring.nslots) {
		if (f->f_flags & O_NONBLOCK) {
			ring.st.dropped_full++;
			mutex_unlock(&ring.lock);
			return -EAGAIN;
		}
		mutex_unlock(&ring.lock);
		if (wait_event_interruptible(ring.wr_wq,
					     READ_ONCE(ring.count) < ring.nslots))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&ring.lock))
			return -ERESTARTSYS;
	}

	s = &ring.slots[ring.head];
	if (copy_from_user(s->data, buf, len)) {
		mutex_unlock(&ring.lock);   /* nothing committed */
		return -EFAULT;
	}
	s->len = len;
	ring.head = (ring.head + 1) % ring.nslots;
	ring.count++;
	ring.st.enqueued++;
	mutex_unlock(&ring.lock);

	wake_up_interruptible(&ring.rd_wq);
	return len;
}

static ssize_t pktring_read(struct file *f, char __user *buf,
			    size_t len, loff_t *off)
{
	struct pkt_slot *s;
	size_t n;

	if (mutex_lock_interruptible(&ring.lock))
		return -ERESTARTSYS;

	while (ring.count == 0) {
		if (f->f_flags & O_NONBLOCK) {
			mutex_unlock(&ring.lock);
			return -EAGAIN;
		}
		mutex_unlock(&ring.lock);
		if (wait_event_interruptible(ring.rd_wq, READ_ONCE(ring.count) > 0))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&ring.lock))
			return -ERESTARTSYS;
	}

	s = &ring.slots[ring.tail];
	n = s->len;
	if (len < n) {
		mutex_unlock(&ring.lock);   /* packet stays queued */
		return -EMSGSIZE;
	}
	if (copy_to_user(buf, s->data, n)) {
		mutex_unlock(&ring.lock);   /* packet stays queued */
		return -EFAULT;
	}
	ring.tail = (ring.tail + 1) % ring.nslots;
	ring.count--;
	ring.st.dequeued++;
	mutex_unlock(&ring.lock);

	wake_up_interruptible(&ring.wr_wq);
	return n;
}

static __poll_t pktring_poll(struct file *f, poll_table *wait)
{
	__poll_t mask = 0;
	u32 count;

	poll_wait(f, &ring.rd_wq, wait);
	poll_wait(f, &ring.wr_wq, wait);

	count = READ_ONCE(ring.count);
	if (count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (count < ring.nslots)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long pktring_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct pktring_stats snap;

	switch (cmd) {
	case PKTRING_IOC_GET_STATS:
		if (mutex_lock_interruptible(&ring.lock))
			return -ERESTARTSYS;
		snap = ring.st;
		snap.depth = ring.count;
		snap.capacity = ring.nslots;
		snap.max_pkt = PKTRING_MAX_PKT;
		snap.reserved = 0;
		mutex_unlock(&ring.lock);
		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		return 0;

	case PKTRING_IOC_RESET:
		if (mutex_lock_interruptible(&ring.lock))
			return -ERESTARTSYS;
		ring.head = ring.tail = ring.count = 0;
		memset(&ring.st, 0, sizeof(ring.st));
		mutex_unlock(&ring.lock);
		wake_up_interruptible(&ring.wr_wq);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations pktring_fops = {
	.owner          = THIS_MODULE,
	.open           = nonseekable_open,
	.read           = pktring_read,
	.write          = pktring_write,
	.poll           = pktring_poll,
	.unlocked_ioctl = pktring_ioctl,
	.compat_ioctl   = pktring_ioctl,
};

static struct miscdevice pktring_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = PKTRING_DEV_NAME,
	.fops  = &pktring_fops,
	.mode  = 0666,   /* world-accessible so the test suite runs unprivileged; tighten for real use */
};

static int __init pktring_init(void)
{
	int ret;

	if (nslots < 1 || nslots > PKTRING_MAX_SLOTS) {
		pr_err("pktring: nslots=%u out of range 1..%u\n",
		       nslots, PKTRING_MAX_SLOTS);
		return -EINVAL;
	}

	mutex_init(&ring.lock);
	init_waitqueue_head(&ring.rd_wq);
	init_waitqueue_head(&ring.wr_wq);
	ring.nslots = nslots;
	ring.slots = kvcalloc(nslots, sizeof(*ring.slots), GFP_KERNEL);
	if (!ring.slots)
		return -ENOMEM;

	ret = misc_register(&pktring_misc);
	if (ret) {
		kvfree(ring.slots);
		return ret;
	}
	pr_info("pktring: loaded, %u slots x %u bytes\n", nslots, PKTRING_MAX_PKT);
	return 0;
}

static void __exit pktring_exit(void)
{
	misc_deregister(&pktring_misc);
	kvfree(ring.slots);
	pr_info("pktring: unloaded\n");
}

module_init(pktring_init);
module_exit(pktring_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Packet-oriented ring buffer char device with user-space test suite");
