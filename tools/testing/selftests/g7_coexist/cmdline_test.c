/* Host regression tests for the exact proc bridge helper; no kernel build. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../fs/proc/g7_coexist_cmdline.h"

#define FORCE "androidboot.force_normal_boot"
#define APPEND " " FORCE "=1"
struct test_case {
	const char *name, *input, *output;
	enum g7_cmdline_status status;
	size_t skip, force;
};

static const struct test_case cases[] = {
	{ "normal", "console=tty0 skip_initramfs rootwait",
	  "console=tty0 want_initramfs rootwait" APPEND, G7_CMDLINE_APPENDED, 1, 0 },
	{ "recovery", "androidboot.mode=recovery", "androidboot.mode=recovery",
	  G7_CMDLINE_UNCHANGED, 0, 0 },
	{ "charger", "skip_initramfs androidboot.mode=charger quiet",
	  "want_initramfs androidboot.mode=charger quiet" APPEND, G7_CMDLINE_APPENDED, 1, 0 },
	{ "prefix-suffix", "xskip_initramfs skip_initramfsx noskip_initramfs",
	  "xskip_initramfs skip_initramfsx noskip_initramfs", G7_CMDLINE_UNCHANGED, 0, 0 },
	{ "quoted-text", "other=\"skip_initramfs " FORCE "=0\" \"skip_initramfs\"",
	  "other=\"skip_initramfs " FORCE "=0\" \"skip_initramfs\"", G7_CMDLINE_UNCHANGED, 0, 0 },
	{ "quoted-key-value", "other=\"" FORCE "=0\" skip_initramfs",
	  "other=\"" FORCE "=0\" want_initramfs" APPEND, G7_CMDLINE_APPENDED, 1, 0 },
	{ "valued-skip", "skip_initramfs=1 skip_initramfs=", "skip_initramfs=1 skip_initramfs=",
	  G7_CMDLINE_UNCHANGED, 0, 0 },
	{ "all-skips", " skip_initramfs\tskip_initramfs  ",
	  " want_initramfs\twant_initramfs  " APPEND, G7_CMDLINE_APPENDED, 2, 0 },
	{ "existing-one", "skip_initramfs " FORCE "=1", "want_initramfs " FORCE "=1",
	  G7_CMDLINE_REWRITTEN, 1, 1 },
	{ "manual-recovery-one", FORCE "=1 androidboot.mode=recovery",
	  FORCE "=1 androidboot.mode=recovery", G7_CMDLINE_UNCHANGED, 0, 1 },
	{ "quoted-one", "skip_initramfs " FORCE "=\"1\"", "want_initramfs " FORCE "=\"1\"",
	  G7_CMDLINE_REWRITTEN, 1, 1 },
	{ "whole-quoted-one", "skip_initramfs \"" FORCE "=1\"", "want_initramfs \"" FORCE "=1\"",
	  G7_CMDLINE_REWRITTEN, 1, 1 },
	{ "existing-zero", "skip_initramfs " FORCE "=0", NULL, G7_CMDLINE_FORCE_CONFLICT, 1, 1 },
	{ "manual-zero", FORCE "=0", NULL, G7_CMDLINE_FORCE_CONFLICT, 0, 1 },
	{ "invalid-force", "skip_initramfs " FORCE "=yes", NULL, G7_CMDLINE_FORCE_CONFLICT, 1, 1 },
	{ "bare-force", "skip_initramfs " FORCE, NULL, G7_CMDLINE_FORCE_CONFLICT, 1, 1 },
	{ "empty-force", "skip_initramfs " FORCE "=", NULL, G7_CMDLINE_FORCE_CONFLICT, 1, 1 },
	{ "duplicate-one", "skip_initramfs " FORCE "=1 " FORCE "=1", NULL,
	  G7_CMDLINE_FORCE_CONFLICT, 1, 2 },
	{ "duplicate-conflict", FORCE "=0 skip_initramfs " FORCE "=1", NULL,
	  G7_CMDLINE_FORCE_CONFLICT, 1, 2 },
	{ "quoted-zero", "skip_initramfs \"" FORCE "=0\"", NULL, G7_CMDLINE_FORCE_CONFLICT, 1, 1 },
	{ "key-substrings", "x" FORCE "=0 " FORCE "x=0 skip_initramfs",
	  "x" FORCE "=0 " FORCE "x=0 want_initramfs" APPEND, G7_CMDLINE_APPENDED, 1, 0 },
	{ "unbalanced-quote", "other=\"skip_initramfs", NULL, G7_CMDLINE_INVALID_INPUT, 0, 0 },
	{ "delimiter", "recovery -- skip_initramfs " FORCE "=0", "recovery -- skip_initramfs " FORCE "=0",
	  G7_CMDLINE_UNCHANGED, 0, 0 },
	{ "delimiter-after-skip", "skip_initramfs -- skip_initramfs",
	  NULL, G7_CMDLINE_INVALID_INPUT, 1, 0 },
	{ "delimiter-existing-force", "skip_initramfs " FORCE "=1 -- skip_initramfs",
	  "want_initramfs " FORCE "=1 -- skip_initramfs", G7_CMDLINE_REWRITTEN, 1, 1 },
	{ "empty", "", "", G7_CMDLINE_UNCHANGED, 0, 0 },
};

int main(void)
{
	char dst[2048], before[2048], source[2048];
	struct g7_cmdline_result r;
	size_t i, n, passed = 0;
	const char embedded[] = "skip_initramfs\0hidden";
	const char unterminated[] = { 'a', 'b', 'c' };

	assert(strlen("skip_initramfs") == strlen("want_initramfs"));
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct test_case *t = &cases[i];

		strcpy(source, t->input);
		memset(dst, 0x5a, sizeof(dst));
		memcpy(before, dst, sizeof(dst));
		r = g7_coexist_cmdline(dst, sizeof(dst), source, strlen(source));
		assert(r.status == t->status);
		assert(r.skip_tokens == t->skip && r.force_tokens == t->force);
		assert(!strcmp(source, t->input));
		if (t->output) {
			assert(!strcmp(dst, t->output));
			assert(r.output_len == strlen(t->output));
		} else {
			assert(!memcmp(dst, before, sizeof(dst)));
		}
		printf("PASS %s status=%u skip=%zu force=%zu output_len=%zu\n",
		       t->name, (unsigned int)r.status, r.skip_tokens,
		       r.force_tokens, r.output_len);
		passed++;
	}
	/* Every capacity through exact fit: no truncation and no partial writes. */
	n = strlen("want_initramfs" APPEND);
	for (i = 0; i <= n + 1; i++) {
		memset(dst, 0x5a, sizeof(dst));
		memcpy(before, dst, sizeof(dst));
		r = g7_coexist_cmdline(dst, i, "skip_initramfs",
				       sizeof("skip_initramfs") - 1);
		if (i <= n) {
			assert(r.status == G7_CMDLINE_NO_SPACE);
			assert(!memcmp(dst, before, sizeof(dst)));
		} else {
			assert(r.status == G7_CMDLINE_APPENDED);
			assert(!strcmp(dst, "want_initramfs" APPEND));
		}
	}
	puts("PASS capacity-sweep status=NO_SPACE until exact fit");
	passed++;
	memset(source, 'x', sizeof(source));
	memset(dst, 0x5a, sizeof(dst));
	memcpy(before, dst, sizeof(dst));
	r = g7_coexist_cmdline(dst, sizeof(dst), source, sizeof(source));
	assert(r.status == G7_CMDLINE_NO_SPACE);
	assert(!memcmp(dst, before, sizeof(dst)));
	puts("PASS oversized-exact-readable-length status=NO_SPACE");
	passed++;
	r = g7_coexist_cmdline(dst, sizeof(dst), embedded, sizeof(embedded) - 1);
	assert(r.status == G7_CMDLINE_INVALID_INPUT);
	assert(!memcmp(dst, before, sizeof(dst)));
	puts("PASS embedded-NUL status=INVALID_INPUT");
	passed++;
	r = g7_coexist_cmdline(dst, sizeof(dst), unterminated, sizeof(unterminated));
	assert(r.status == G7_CMDLINE_UNCHANGED && !strcmp(dst, "abc"));
	puts("PASS no-input-terminator-required status=UNCHANGED");
	passed++;
	printf("PASS total=%zu (plus %zu capacity checks)\n", passed, n + 2);
	return 0;
}
