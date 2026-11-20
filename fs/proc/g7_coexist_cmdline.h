/* SPDX-License-Identifier: GPL-2.0 */
#ifndef G7_COEXIST_CMDLINE_H
#define G7_COEXIST_CMDLINE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stddef.h>
#include <string.h>
#endif

/* No allocation, logging or Linux-only API: host tests use this exact code. */
enum g7_cmdline_status {
	G7_CMDLINE_UNCHANGED,
	G7_CMDLINE_REWRITTEN,
	G7_CMDLINE_APPENDED,
	G7_CMDLINE_INVALID_INPUT,
	G7_CMDLINE_FORCE_CONFLICT,
	G7_CMDLINE_NO_SPACE
};

struct g7_cmdline_result {
	enum g7_cmdline_status status;
	size_t skip_tokens;
	size_t force_tokens;
	size_t output_len;
};

static inline int g7_cmdline_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

/* Kernel next_arg uses double quotes, without backslash escaping. */
static inline size_t g7_cmdline_token_end(const char *src, size_t len,
					size_t start, int *quoted)
{
	size_t i;

	*quoted = 0;
	for (i = start; i < len; i++) {
		if (g7_cmdline_space(src[i]) && !*quoted)
			break;
		if (src[i] == '"')
			*quoted = !*quoted;
	}
	return i;
}

/*
 * src_len is the exact readable byte length, excluding any terminator.
 * Embedded NULs are rejected. src and dst must not overlap. On any error,
 * dst is untouched; callers must expose src, not a partial normal-boot view.
 * All lengths include neither the output NUL nor the proc newline.
 */
static inline struct g7_cmdline_result
 g7_coexist_cmdline(char *dst, size_t capacity, const char *src, size_t src_len)
{
	static const char skip[] = "skip_initramfs";
	static const char want[] = "want_initramfs";
	static const char force[] = "androidboot.force_normal_boot";
	static const char append[] = " androidboot.force_normal_boot=1";
	struct g7_cmdline_result r = { G7_CMDLINE_UNCHANGED, 0, 0, 0 };
	size_t i, start, end, key_start, key_end, val_start, val_end;
	int quoted, conflict = 0, setup_ended = 0;

	if (!dst || !src || memchr(src, '\0', src_len)) {
		r.status = G7_CMDLINE_INVALID_INPUT;
		return r;
	}
	for (i = 0; i < src_len;) {
		if (g7_cmdline_space(src[i])) {
			i++;
			continue;
		}
		start = i;
		end = g7_cmdline_token_end(src, src_len, start, &quoted);
		if (quoted) {
			r.status = G7_CMDLINE_INVALID_INPUT;
			return r;
		}
		i = end;
		/* Kernel setup parsing stops at the standalone -- delimiter. */
		if (end - start == 2 && !memcmp(src + start, "--", 2)) {
			setup_ended = 1;
			break;
		}
		if (end - start == sizeof(skip) - 1 &&
		    !memcmp(src + start, skip, sizeof(skip) - 1))
			r.skip_tokens++;

		/* A whole quoted argument is still a key, not another key's value. */
		key_start = start;
		key_end = end;
		if (src[key_start] == '"' && src[key_end - 1] == '"') {
			key_start++;
			key_end--;
		}
		if (key_end - key_start < sizeof(force) - 1 ||
		    memcmp(src + key_start, force, sizeof(force) - 1))
			continue;
		val_start = key_start + sizeof(force) - 1;
		if (val_start < key_end && src[val_start] != '=')
			continue;
		r.force_tokens++;
		if (val_start == key_end) {
			conflict = 1;
			continue;
		}
		val_start++;
		val_end = key_end;
		if (val_end - val_start >= 2 && src[val_start] == '"' &&
		    src[val_end - 1] == '"') {
			val_start++;
			val_end--;
		}
		if (val_end - val_start != 1 || src[val_start] != '1')
			conflict = 1;
	}
	if (conflict || r.force_tokens > 1) {
		r.status = G7_CMDLINE_FORCE_CONFLICT;
		return r;
	}
	/* Appending after -- would create an init argument, not a boot key. */
	if (setup_ended && r.skip_tokens && !r.force_tokens) {
		r.status = G7_CMDLINE_INVALID_INPUT;
		return r;
	}
	if (src_len >= capacity ||
	    (r.skip_tokens && !r.force_tokens &&
	     sizeof(append) - 1 > capacity - src_len - 1)) {
		r.status = G7_CMDLINE_NO_SPACE;
		return r;
	}
	memcpy(dst, src, src_len);
	if (r.skip_tokens) {
		for (i = 0; i < src_len;) {
			if (g7_cmdline_space(src[i])) {
				i++;
				continue;
			}
			start = i;
			i = g7_cmdline_token_end(src, src_len, start, &quoted);
			if (i - start == 2 && !memcmp(src + start, "--", 2))
				break;
			if (i - start == sizeof(skip) - 1 &&
			    !memcmp(src + start, skip, sizeof(skip) - 1))
				memcpy(dst + start, want, sizeof(want) - 1);
		}
		r.status = G7_CMDLINE_REWRITTEN;
	}
	r.output_len = src_len;
	if (r.skip_tokens && !r.force_tokens) {
		memcpy(dst + src_len, append, sizeof(append) - 1);
		r.output_len += sizeof(append) - 1;
		r.status = G7_CMDLINE_APPENDED;
	}
	dst[r.output_len] = '\0';
	return r;
}

#endif /* G7_COEXIST_CMDLINE_H */
