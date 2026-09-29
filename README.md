# pktring — Linux kernel module with user-space tests

A packet-oriented ring buffer character device (`/dev/pktring`) that models the
software side of a NIC TX/RX descriptor ring, plus a user-space test suite that
checks its correctness and error handling.

## Design
- One `write()` = one packet (1..2048 bytes); one `read()` = one packet. Message boundaries are preserved.
- Fixed ring of `nslots` packets (module param, 1..4096, default 64), guarded by a mutex.
- Full ring / empty ring: block on wait queues, or return `-EAGAIN` with `O_NONBLOCK`.
- `poll()` support (`POLLIN` when data queued, `POLLOUT` when space available).
- `ioctl`: `PKTRING_IOC_GET_STATS` (enqueued, dequeued, dropped_full, bad_write, depth, capacity) and `PKTRING_IOC_RESET`.

## Error contract (each item is tested)
| Operation | Condition | Result |
|---|---|---|
| write | length 0 | `EINVAL` |
| write | length > 2048 | `EMSGSIZE` |
| write | bad user pointer | `EFAULT`, nothing enqueued |
| write | ring full, `O_NONBLOCK` | `EAGAIN`, `dropped_full++` |
| read | ring empty, `O_NONBLOCK` | `EAGAIN` |
| read | buffer smaller than packet | `EMSGSIZE`, packet stays queued |
| read | bad user pointer | `EFAULT`, packet stays queued |
| ioctl | unknown command | `ENOTTY` |
| ioctl | bad user pointer | `EFAULT` |
| lseek | any | `ESPIPE` |
| insmod | `nslots` = 0 or > 4096 | `EINVAL` |

## Test suite (`pktring_test.c`, 17 tests)
Round-trip integrity, FIFO order and message boundaries, max-size packet, invalid lengths,
empty/full non-blocking behaviour, small-buffer and bad-pointer paths (no data loss),
index wrap-around, stats/reset/unknown ioctl, non-seekable, two fds sharing one ring,
poll semantics, blocking read/write wake-ups (threads), and a 100,000-packet
producer/consumer stress test verifying ordering, length and checksum of every packet.

## Build and run
```
sudo apt install build-essential linux-headers-$(uname -r)
make                 # builds pktring.ko and pktring_test
sudo ./run_tests.sh  # loads with nslots=64, 1 and 7, runs the suite, checks bad nslots is rejected
```
Or manually:
```
sudo insmod pktring.ko nslots=16
./pktring_test
sudo rmmod pktring
```
Running with `nslots=1` and `7` exercises the edge cases (full after one packet, odd wrap-around).

## Notes
- Device node is mode 0666 so tests run unprivileged; restrict it for real use.
- Targets recent kernels (5.x/6.x). Verify the build on your own kernel before listing it as tested.
