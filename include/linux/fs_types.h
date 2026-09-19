/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FS_TYPES_H
#define _LINUX_FS_TYPES_H

#include <linux/types.h>

/*
 * 取自 Linux 5.15 的 include/linux/fs_types.h。
 * 4.9 的 include/linux/fs.h 已经有 DT_*（d_type 那套），所以这里只补盘上用的
 * FT_* 一套，并用 #ifndef 兜住 S_DT*，避免重复定义。
 * 5.15 把三个转换函数放在 fs/fs_types.c，这里内联实现，免得为一张转换表新增 .c。
 */

#ifndef S_DT_SHIFT
#define S_DT_SHIFT	12
#define S_DT(mode)	(((mode) & S_IFMT) >> S_DT_SHIFT)
#define S_DT_MASK	(S_IFMT >> S_DT_SHIFT)
#endif

/* fs on-disk file types (低 3 位表示 POSIX 文件类型，其余位留给文件系统私用) */
#define FT_UNKNOWN	0
#define FT_REG_FILE	1
#define FT_DIR		2
#define FT_CHRDEV	3
#define FT_BLKDEV	4
#define FT_FIFO		5
#define FT_SOCK		6
#define FT_SYMLINK	7
#define FT_MAX		8

static inline unsigned char fs_ftype_to_dtype(unsigned int filetype)
{
	static const unsigned char table[FT_MAX] = {
		DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
	};

	if (filetype >= FT_MAX)
		return DT_UNKNOWN;
	return table[filetype];
}

static inline unsigned char fs_umode_to_ftype(umode_t mode)
{
	return (mode >> S_DT_SHIFT) & S_DT_MASK;
}

static inline unsigned char fs_umode_to_dtype(umode_t mode)
{
	return fs_ftype_to_dtype(fs_umode_to_ftype(mode));
}

#endif /* _LINUX_FS_TYPES_H */
