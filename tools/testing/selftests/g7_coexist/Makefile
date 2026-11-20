HOSTCC ?= cc
CFLAGS := -std=gnu89 -O2 -Wall -Wextra -Werror
OUTPUT ?= .
TEST := $(OUTPUT)/cmdline_test

.PHONY: all run_tests clean
all: $(TEST)

$(TEST): cmdline_test.c ../../../../fs/proc/g7_coexist_cmdline.h
	mkdir -p $(OUTPUT)
	$(HOSTCC) $(CFLAGS) $< -o $@

run_tests: $(TEST)
	$(TEST)

clean:
	rm -f $(TEST)
