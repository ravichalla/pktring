#!/bin/bash
# Load the module, run the user-space suite (default + small ring), unload.
set -u
cd "$(dirname "$0")"
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo ./run_tests.sh)"; exit 2; }

rc=0
for slots in 64 1 7; do
    echo "=== nslots=$slots ==="
    rmmod pktring 2>/dev/null
    insmod ./pktring.ko nslots=$slots || { echo "insmod failed"; exit 1; }
    sleep 0.3                     # let udev create /dev/pktring
    ./pktring_test || rc=1
    rmmod pktring || rc=1
done

echo "=== invalid nslots must be rejected ==="
if insmod ./pktring.ko nslots=0 2>/dev/null; then
    echo "FAIL: nslots=0 accepted"; rmmod pktring; rc=1
else
    echo "OK: nslots=0 rejected"
fi

echo "--- kernel log ---"; dmesg | tail -n 10
exit $rc
