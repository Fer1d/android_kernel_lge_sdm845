#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#ifdef CONFIG_PROC_CMDLINE_APPEND_FORCE_NORMAL_BOOT
#include <asm/setup.h>
#include "g7_coexist_cmdline.h"

/* Construct once at init: readers never modify the saved command line. */
static char g7_cmdline_view[COMMAND_LINE_SIZE];
static int g7_cmdline_view_valid;
#endif

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_PROC_CMDLINE_APPEND_FORCE_NORMAL_BOOT
	if (g7_cmdline_view_valid) {
		seq_printf(m, "%s\n", g7_cmdline_view);
		return 0;
	}
#endif
	seq_printf(m, "%s\n", saved_command_line);
	return 0;
}

static int cmdline_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cmdline_proc_show, NULL);
}

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_cmdline_init(void)
{
#ifdef CONFIG_PROC_CMDLINE_APPEND_FORCE_NORMAL_BOOT
	struct g7_cmdline_result result;
	size_t len = strnlen(saved_command_line, COMMAND_LINE_SIZE);

	if (len == COMMAND_LINE_SIZE) {
		pr_warn("g7 cmdline: oversized input; exposing original command line\n");
	} else {
		result = g7_coexist_cmdline(g7_cmdline_view,
					  sizeof(g7_cmdline_view),
					  saved_command_line, len);
		if (result.status <= G7_CMDLINE_APPENDED)
			g7_cmdline_view_valid = 1;
		else
			pr_warn("g7 cmdline: bridge rejected (status=%u skip=%zu force=%zu); exposing original command line\n",
				(unsigned int)result.status, result.skip_tokens,
				result.force_tokens);
	}
#endif
	proc_create("cmdline", 0, NULL, &cmdline_proc_fops);
	return 0;
}
fs_initcall(proc_cmdline_init);
