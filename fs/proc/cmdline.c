#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#if defined(CONFIG_INITRAMFS_IGNORE_SKIP_FLAG) || \
	defined(CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT)
#include <asm/setup.h>
#include <linux/slab.h>

#define INITRAMFS_STR_FIND "skip_initramf"
#endif // CONFIG_INITRAMFS_IGNORE_SKIP_FLAG || CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT

#ifdef CONFIG_INITRAMFS_IGNORE_SKIP_FLAG
#define INITRAMFS_STR_REPLACE "want_initramf"
#define INITRAMFS_STR_LEN (sizeof(INITRAMFS_STR_FIND) - 1)
#endif // CONFIG_INITRAMFS_IGNORE_SKIP_FLAG

#ifdef CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT
#define ANDROID_FORCE_NORMAL_BOOT_STR "androidboot.force_normal_boot=1"
#endif // CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT

#if defined(CONFIG_INITRAMFS_IGNORE_SKIP_FLAG) || \
	defined(CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT)
static char proc_command_line[COMMAND_LINE_SIZE];

static void proc_command_line_init(void)
{
	char *offset_addr;
	char *proc_command_line_tail;

	strcpy(proc_command_line, saved_command_line);

#ifdef CONFIG_INITRAMFS_IGNORE_SKIP_FLAG
	offset_addr = strstr(proc_command_line, INITRAMFS_STR_FIND);
	if (offset_addr)
		memcpy(offset_addr, INITRAMFS_STR_REPLACE, INITRAMFS_STR_LEN);
#endif // CONFIG_INITRAMFS_IGNORE_SKIP_FLAG

#ifdef CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT
	if (strstr(saved_command_line, INITRAMFS_STR_FIND) &&
	    strlen(proc_command_line) + sizeof(ANDROID_FORCE_NORMAL_BOOT_STR) <
		    COMMAND_LINE_SIZE) {
		// point proc_command_line_tail to the null terminator of the cmdline
		proc_command_line_tail = proc_command_line + strlen(proc_command_line);
		memcpy(proc_command_line_tail, " ", 1);
		memcpy(proc_command_line_tail + 1, ANDROID_FORCE_NORMAL_BOOT_STR,
		       sizeof(ANDROID_FORCE_NORMAL_BOOT_STR));
	}
#endif // CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT
}
#endif // CONFIG_INITRAMFS_IGNORE_SKIP_FLAG || CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#if defined(CONFIG_INITRAMFS_IGNORE_SKIP_FLAG) || \
	defined(CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT)
	seq_printf(m, "%s\n", proc_command_line);
#else
	seq_printf(m, "%s\n", saved_command_line);
#endif
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
#if defined(CONFIG_INITRAMFS_IGNORE_SKIP_FLAG) || \
	defined(CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT)
	proc_command_line_init();
#endif

	proc_create("cmdline", 0, NULL, &cmdline_proc_fops);
	return 0;
}
fs_initcall(proc_cmdline_init);
