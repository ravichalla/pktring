/* SPDX-License-Identifier: GPL-2.0 */
/* Shared kernel/user ABI for the pktring character device. */
#ifndef PKTRING_H
#define PKTRING_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define PKTRING_DEV_NAME   "pktring"
#define PKTRING_MAX_PKT    2048u   /* max bytes per packet (one write = one packet) */
#define PKTRING_MAX_SLOTS  4096u
#define PKTRING_DEF_SLOTS  64u

struct pktring_stats {
	__u64 enqueued;      /* packets accepted by write() */
	__u64 dequeued;      /* packets returned by read() */
	__u64 dropped_full;  /* non-blocking writes rejected because ring was full */
	__u64 bad_write;     /* writes rejected: zero or oversize length */
	__u32 depth;         /* packets currently queued */
	__u32 capacity;      /* number of slots */
	__u32 max_pkt;       /* max packet size in bytes */
	__u32 reserved;
};

#define PKTRING_IOC_MAGIC      'p'
#define PKTRING_IOC_GET_STATS  _IOR(PKTRING_IOC_MAGIC, 1, struct pktring_stats)
#define PKTRING_IOC_RESET      _IO(PKTRING_IOC_MAGIC, 2) /* drop all packets, zero counters */

#endif /* PKTRING_H */
