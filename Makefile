obj-m += pktring.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module pktring_test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

pktring_test: pktring_test.c pktring.h
	$(CC) -O2 -g -Wall -Wextra -pthread -o $@ $<

# Build, load, run the suite, unload. Needs root for insmod/rmmod.
check: all
	./run_tests.sh

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f pktring_test

.PHONY: all module check clean
