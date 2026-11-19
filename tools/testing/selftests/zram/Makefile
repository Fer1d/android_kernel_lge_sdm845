all:

TEST_PROGS := zram.sh
TEST_FILES := zram01.sh zram02.sh zram_lib.sh

include ../lib.mk

clean:
	$(RM) err.log

# Deliberately separate from run_tests: these never invoke zram.sh or devices.
PYTHON ?= python3
HOSTCC ?= gcc
.PHONY: host-test host-test-sanitize observer-host-test observer-host-test-sanitize
observer-host-test:
	HOSTCC="$(HOSTCC)" $(PYTHON) -I ./zram_pool_observer_host_test.py

observer-host-test-sanitize:
	HOSTCC="$(HOSTCC)" $(PYTHON) -I ./zram_pool_observer_host_test.py --sanitize

host-test:
	HOSTCC="$(HOSTCC)" $(PYTHON) -I ./zram_writeback_host_test.py

host-test-sanitize:
	HOSTCC="$(HOSTCC)" $(PYTHON) -I ./zram_writeback_host_test.py --sanitize
