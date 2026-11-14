# Host-only: intentionally not lib.mk (no kernel/device dependency).
PYTHON ?= python3
HOSTCC ?= cc
SRC ?=
OUT ?=
SANITIZE ?= 0
BASELINE_SOURCE ?=

.PHONY: all run_tests
all:
	@printf '%s\n' 'Use make run_tests SRC=/absolute/kernel OUT=/absolute/output HOSTCC=cc'

run_tests:
	@test -n "$(SRC)" -a -n "$(OUT)" || { printf '%s\n' 'SRC and OUT are required' >&2; exit 2; }
	$(PYTHON) -I $(CURDIR)/test_sync_init.py --src "$(SRC)" --out "$(OUT)" --hostcc "$(HOSTCC)" $(if $(filter 1,$(SANITIZE)),--sanitize) $(if $(BASELINE_SOURCE),--baseline-source "$(BASELINE_SOURCE)")
