/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MYFS_IOCTL_H
#define MYFS_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MYFS_NAME_MAX	63

struct myfs_file_meta {
	__u32 index;
	__u32 crc32;
	char  name[MYFS_NAME_MAX + 1];
};

struct myfs_meta_req {
	__u64 entries;
	__u32 count;
	__u32 pad;
};

struct myfs_sector_map {
	char  name[MYFS_NAME_MAX + 1];
	__u32 index;
	__u32 sector_size;
	__u32 start;
	__u32 count;
};

#define MYFS_IOC_MAGIC		0xBE
#define MYFS_IOC_ZERO_ALL	_IO(MYFS_IOC_MAGIC, 1)
#define MYFS_IOC_ERASE_FS	_IO(MYFS_IOC_MAGIC, 2)
#define MYFS_IOC_GET_META	_IOWR(MYFS_IOC_MAGIC, 3, struct myfs_meta_req)
#define MYFS_IOC_GET_SECTOR_MAP	_IOWR(MYFS_IOC_MAGIC, 4, struct myfs_sector_map)

#endif
