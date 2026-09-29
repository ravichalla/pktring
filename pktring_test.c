// SPDX-License-Identifier: GPL-2.0
/*
 * User-space test suite for the pktring kernel module.
 * Usage: ./pktring_test [/dev/pktring]
 * Exit status 0 if all tests pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "pktring.h"

static const char *dev_path = "/dev/" PKTRING_DEV_NAME;
/* Deliberately invalid user address (volatile so the compiler cannot warn/optimize). */
static void *volatile BAD = (void *)0x10;

#define CHECK(cond) do { if (!(cond)) { \
	fprintf(stderr, "    FAIL %s:%d: %s (errno=%d %s)\n", \
		__FILE__, __LINE__, #cond, errno, strerror(errno)); return -1; } } while (0)

/* Expect a syscall expression to fail with a specific errno. */
#define CHECK_ERR(expr, want) do { errno = 0; long _r = (long)(expr); \
	if (_r != -1 || errno != (want)) { \
	fprintf(stderr, "    FAIL %s:%d: %s -> %ld, errno=%d (%s), wanted errno=%d (%s)\n", \
		__FILE__, __LINE__, #expr, _r, errno, strerror(errno), \
		(want), strerror(want)); return -1; } } while (0)

static int open_dev(int flags)
{
	int fd = open(dev_path, flags);
	if (fd < 0) { perror("open"); return -1; }
	if (ioctl(fd, PKTRING_IOC_RESET) < 0) { perror("reset"); close(fd); return -1; }
	return fd;
}

static int get_stats(int fd, struct pktring_stats *st)
{
	return ioctl(fd, PKTRING_IOC_GET_STATS, st);
}

/* ---------------- tests ---------------- */

static int test_open_close(void)
{
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	CHECK(close(fd) == 0);
	return 0;
}

static int test_roundtrip(void)
{
	char in[100], out[PKTRING_MAX_PKT];
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	for (int i = 0; i < (int)sizeof(in); i++) in[i] = (char)(i * 7 + 3);
	CHECK(write(fd, in, sizeof(in)) == (ssize_t)sizeof(in));
	CHECK(read(fd, out, sizeof(out)) == (ssize_t)sizeof(in));
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	close(fd);
	return 0;
}

static int test_fifo_and_boundaries(void)
{
	int fd = open_dev(O_RDWR);
	uint8_t buf[PKTRING_MAX_PKT];
	CHECK(fd >= 0);
	/* Different sizes: message boundaries must be preserved, order FIFO. */
	for (int i = 1; i <= 10; i++) {
		memset(buf, i, i * 10);
		CHECK(write(fd, buf, i * 10) == i * 10);
	}
	for (int i = 1; i <= 10; i++) {
		CHECK(read(fd, buf, sizeof(buf)) == i * 10);
		for (int j = 0; j < i * 10; j++) CHECK(buf[j] == i);
	}
	close(fd);
	return 0;
}

static int test_max_size_packet(void)
{
	static uint8_t in[PKTRING_MAX_PKT], out[PKTRING_MAX_PKT];
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	for (unsigned i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i ^ (i >> 8));
	CHECK(write(fd, in, PKTRING_MAX_PKT) == PKTRING_MAX_PKT);
	CHECK(read(fd, out, sizeof(out)) == PKTRING_MAX_PKT);
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	close(fd);
	return 0;
}

static int test_write_invalid_length(void)
{
	static char big[PKTRING_MAX_PKT + 1];
	struct pktring_stats st;
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	CHECK_ERR(write(fd, big, 0), EINVAL);
	CHECK_ERR(write(fd, big, PKTRING_MAX_PKT + 1), EMSGSIZE);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.depth == 0);          /* nothing enqueued */
	CHECK(st.enqueued == 0);
	CHECK(st.bad_write == 2);
	close(fd);
	return 0;
}

static int test_read_empty_nonblock(void)
{
	char buf[64];
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	CHECK(fd >= 0);
	CHECK_ERR(read(fd, buf, sizeof(buf)), EAGAIN);
	close(fd);
	return 0;
}

static int test_read_small_buffer_keeps_packet(void)
{
	char in[200], small[50], out[200];
	struct pktring_stats st;
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	memset(in, 0x5a, sizeof(in));
	CHECK(write(fd, in, sizeof(in)) == (ssize_t)sizeof(in));
	CHECK_ERR(read(fd, small, sizeof(small)), EMSGSIZE);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.depth == 1 && st.dequeued == 0);   /* not consumed */
	CHECK(read(fd, out, sizeof(out)) == (ssize_t)sizeof(in));  /* still readable */
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	close(fd);
	return 0;
}

static int test_bad_user_pointer(void)
{
	char in[64] = "hello", out[64];
	struct pktring_stats st;
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	/* write from an invalid address must fail without enqueuing */
	CHECK_ERR(write(fd, BAD, 32), EFAULT);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.depth == 0 && st.enqueued == 0);
	/* read into an invalid address must fail without consuming */
	CHECK(write(fd, in, sizeof(in)) == (ssize_t)sizeof(in));
	CHECK_ERR(read(fd, BAD, sizeof(in)), EFAULT);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.depth == 1 && st.dequeued == 0);
	CHECK(read(fd, out, sizeof(out)) == (ssize_t)sizeof(in));
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	/* bad pointer for ioctl */
	CHECK_ERR(ioctl(fd, PKTRING_IOC_GET_STATS, BAD), EFAULT);
	close(fd);
	return 0;
}

static int test_full_ring_nonblock(void)
{
	char pkt[64] = {0};
	struct pktring_stats st;
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	CHECK(fd >= 0);
	CHECK(get_stats(fd, &st) == 0);
	uint32_t cap = st.capacity;
	CHECK(cap >= 1);
	for (uint32_t i = 0; i < cap; i++)
		CHECK(write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt));
	CHECK_ERR(write(fd, pkt, sizeof(pkt)), EAGAIN);
	CHECK_ERR(write(fd, pkt, sizeof(pkt)), EAGAIN);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.depth == cap && st.enqueued == cap && st.dropped_full == 2);
	/* draining one slot makes room again */
	CHECK(read(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt));
	CHECK(write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt));
	close(fd);
	return 0;
}

static int test_wraparound(void)
{
	/* Push several times the capacity through the ring to exercise index wrap. */
	struct pktring_stats st;
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	CHECK(fd >= 0);
	CHECK(get_stats(fd, &st) == 0);
	uint32_t total = st.capacity * 5 + 3;
	for (uint32_t i = 0; i < total; i++) {
		uint32_t v = i, r = 0;
		CHECK(write(fd, &v, sizeof(v)) == sizeof(v));
		CHECK(read(fd, &r, sizeof(r)) == sizeof(r));
		CHECK(r == i);
	}
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.enqueued == total && st.dequeued == total && st.depth == 0);
	close(fd);
	return 0;
}

static int test_ioctl_stats_reset_unknown(void)
{
	char pkt[32] = {0};
	struct pktring_stats st;
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.max_pkt == PKTRING_MAX_PKT && st.depth == 0);
	for (int i = 0; i < 5; i++) CHECK(write(fd, pkt, sizeof(pkt)) > 0);
	CHECK(read(fd, pkt, sizeof(pkt)) > 0);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.enqueued == 5 && st.dequeued == 1 && st.depth == 4);
	CHECK(ioctl(fd, PKTRING_IOC_RESET) == 0);
	CHECK(get_stats(fd, &st) == 0);
	CHECK(st.enqueued == 0 && st.dequeued == 0 && st.depth == 0);
	CHECK_ERR(ioctl(fd, _IO('p', 99)), ENOTTY);
	close(fd);
	return 0;
}

static int test_not_seekable(void)
{
	int fd = open_dev(O_RDWR);
	CHECK(fd >= 0);
	CHECK_ERR(lseek(fd, 0, SEEK_SET), ESPIPE);
	close(fd);
	return 0;
}

static int test_two_fds_share_ring(void)
{
	char in[] = "shared", out[16];
	int a = open_dev(O_RDWR), b = open_dev(O_RDWR);
	CHECK(a >= 0 && b >= 0);
	CHECK(write(a, in, sizeof(in)) == (ssize_t)sizeof(in));
	CHECK(read(b, out, sizeof(out)) == (ssize_t)sizeof(in));
	CHECK(strcmp(out, in) == 0);
	close(a); close(b);
	return 0;
}

static int test_poll_semantics(void)
{
	char pkt[16] = {0};
	struct pktring_stats st;
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	CHECK(fd >= 0);
	struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };

	CHECK(poll(&p, 1, 0) == 1);                    /* empty: writable only */
	CHECK((p.revents & POLLOUT) && !(p.revents & POLLIN));

	CHECK(write(fd, pkt, sizeof(pkt)) > 0);
	CHECK(get_stats(fd, &st) == 0);
	if (st.capacity > 1) {                         /* partial: both (needs >1 slot) */
		CHECK(poll(&p, 1, 0) == 1);
		CHECK((p.revents & POLLOUT) && (p.revents & POLLIN));
	}
	for (uint32_t i = 1; i < st.capacity; i++) CHECK(write(fd, pkt, sizeof(pkt)) > 0);
	CHECK(poll(&p, 1, 0) == 1);                    /* full: readable only */
	CHECK((p.revents & POLLIN) && !(p.revents & POLLOUT));
	close(fd);
	return 0;
}

/* ---- blocking behaviour ---- */

static void *delayed_writer(void *arg)
{
	int fd = *(int *)arg;
	usleep(150 * 1000);
	char msg[] = "wakeup";
	if (write(fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg)) perror("delayed write");
	return NULL;
}

static int test_blocking_read_wakes(void)
{
	char out[32];
	pthread_t t;
	int rfd = open_dev(O_RDWR), wfd = open_dev(O_RDWR);
	CHECK(rfd >= 0 && wfd >= 0);
	alarm(20);
	CHECK(pthread_create(&t, NULL, delayed_writer, &wfd) == 0);
	CHECK(read(rfd, out, sizeof(out)) == 7);       /* blocks until writer runs */
	CHECK(strcmp(out, "wakeup") == 0);
	pthread_join(t, NULL);
	alarm(0);
	close(rfd); close(wfd);
	return 0;
}

static void *delayed_reader(void *arg)
{
	int fd = *(int *)arg;
	char buf[PKTRING_MAX_PKT];
	usleep(150 * 1000);
	if (read(fd, buf, sizeof(buf)) < 0) perror("delayed read");
	return NULL;
}

static int test_blocking_write_wakes(void)
{
	char pkt[8] = {0};
	struct pktring_stats st;
	pthread_t t;
	int wfd = open_dev(O_RDWR), rfd = open_dev(O_RDWR);
	CHECK(wfd >= 0 && rfd >= 0);
	CHECK(get_stats(wfd, &st) == 0);
	for (uint32_t i = 0; i < st.capacity; i++) CHECK(write(wfd, pkt, sizeof(pkt)) > 0);
	alarm(20);
	CHECK(pthread_create(&t, NULL, delayed_reader, &rfd) == 0);
	CHECK(write(wfd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt));  /* blocks until reader frees a slot */
	pthread_join(t, NULL);
	alarm(0);
	close(rfd); close(wfd);
	return 0;
}

/* ---- concurrent producer/consumer stress with integrity check ---- */

#define STRESS_PKTS 100000u

struct hdr { uint32_t seq, len, sum; };

static uint32_t payload_sum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i < n; i++) s = s * 31 + p[i];
	return s;
}

static void *producer(void *arg)
{
	int fd = *(int *)arg;
	uint8_t buf[PKTRING_MAX_PKT];
	unsigned rng = 12345;
	for (uint32_t seq = 0; seq < STRESS_PKTS; seq++) {
		rng = rng * 1103515245u + 12345u;
		uint32_t len = sizeof(struct hdr) + (rng >> 8) % (PKTRING_MAX_PKT - sizeof(struct hdr) + 1);
		for (uint32_t i = sizeof(struct hdr); i < len; i++) buf[i] = (uint8_t)(seq + i);
		struct hdr h = { seq, len, payload_sum(buf + sizeof(h), len - sizeof(h)) };
		memcpy(buf, &h, sizeof(h));
		if (write(fd, buf, len) != (ssize_t)len) { perror("producer write"); return BAD; }
	}
	return NULL;
}

static int test_stress_spsc(void)
{
	uint8_t buf[PKTRING_MAX_PKT];
	pthread_t t;
	struct pktring_stats st;
	int wfd = open_dev(O_RDWR), rfd = open_dev(O_RDWR);
	CHECK(wfd >= 0 && rfd >= 0);
	alarm(60);
	CHECK(pthread_create(&t, NULL, producer, &wfd) == 0);
	for (uint32_t expect = 0; expect < STRESS_PKTS; expect++) {
		ssize_t n = read(rfd, buf, sizeof(buf));
		CHECK(n >= (ssize_t)sizeof(struct hdr));
		struct hdr h; memcpy(&h, buf, sizeof(h));
		CHECK(h.seq == expect);                     /* in order, nothing lost or duplicated */
		CHECK(h.len == (uint32_t)n);                /* boundaries preserved */
		CHECK(h.sum == payload_sum(buf + sizeof(h), n - sizeof(h)));  /* data intact */
	}
	void *rc; pthread_join(t, &rc);
	alarm(0);
	CHECK(rc == NULL);
	CHECK(get_stats(rfd, &st) == 0);
	CHECK(st.enqueued == STRESS_PKTS && st.dequeued == STRESS_PKTS && st.depth == 0);
	close(rfd); close(wfd);
	return 0;
}

/* ---------------- runner ---------------- */

struct test { const char *name; int (*fn)(void); };
#define T(f) { #f, f }

int main(int argc, char **argv)
{
	if (argc > 1) dev_path = argv[1];
	struct test tests[] = {
		T(test_open_close), T(test_roundtrip), T(test_fifo_and_boundaries),
		T(test_max_size_packet), T(test_write_invalid_length),
		T(test_read_empty_nonblock), T(test_read_small_buffer_keeps_packet),
		T(test_bad_user_pointer), T(test_full_ring_nonblock), T(test_wraparound),
		T(test_ioctl_stats_reset_unknown), T(test_not_seekable),
		T(test_two_fds_share_ring), T(test_poll_semantics),
		T(test_blocking_read_wakes), T(test_blocking_write_wakes),
		T(test_stress_spsc),
	};
	int n = sizeof(tests) / sizeof(tests[0]), failed = 0;

	signal(SIGPIPE, SIG_IGN);
	printf("pktring test suite on %s\n", dev_path);
	for (int i = 0; i < n; i++) {
		int rc = tests[i].fn();
		printf("[%s] %s\n", rc == 0 ? " OK " : "FAIL", tests[i].name);
		if (rc) failed++;
	}
	printf("\n%d/%d passed\n", n - failed, n);
	return failed ? 1 : 0;
}
