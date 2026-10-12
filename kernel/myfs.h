/* SPDX-License-Identifier: GPL-2.0 */

#ifndef MYFS_H
#define MYFS_H

#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "myfs_ioctl.h"

#define MYFS_MAGIC		0x5346594D
#define MYFS_VERSION		3
#define MYFS_SB_SIZE		512
#define MYFS_ROOT_INO		1
#define MYFS_FIRST_FILE_INO	2
#define MYFS_NAME_PREFIX	"file"
#define MYFS_NAME_FMT		MYFS_NAME_PREFIX "%05u"
#define MYFS_MAX_FILE_BYTES	(BIO_MAX_VECS * PAGE_SIZE)

struct myfs_super_block {
	__u32 magic;
	__u32 version;
	__u32 sector_size;
	__u32 total_sectors;
	__u32 sb_offset_1;
	__u32 sb_offset_2;
	__u32 max_name_len;
	__u32 file_sectors;
	__u32 num_files;
	__u8  reserved[472];
	__u32 checksum;
};

struct myfs_file {
	char name[MYFS_NAME_MAX + 1];
	u32 start;
	u32 sectors;
};

struct myfs_info {
	struct myfs_super_block sb;
	struct myfs_file *files;
	struct file *bdev_file;
	struct block_device *bdev;
	unsigned int sector_size;
	struct mutex lock;
	bool erased;
};

extern char *myfs_dev;
extern uint sb_offset_1;
extern uint sb_offset_2;
extern uint max_name_len;
extern uint file_sectors;

static inline struct myfs_info *MYFS_INFO(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline size_t myfs_file_bytes(const struct myfs_info *fsi)
{
	return (size_t)fsi->sb.file_sectors * fsi->sector_size;
}

int myfs_rw(struct myfs_info *fsi, u32 sector, void *buf, size_t len,
	    blk_opf_t opf);
int myfs_file_rw(struct myfs_info *fsi, u32 idx, void *buf, blk_opf_t opf);
int myfs_zero_files(struct myfs_info *fsi);
u32 myfs_crc32(const void *buf, size_t len);

u32 myfs_layout(const struct myfs_super_block *sb, struct myfs_file *files);
int myfs_check_geometry(const struct myfs_info *fsi,
			const struct myfs_super_block *sb);

int myfs_format(struct myfs_info *fsi);
int myfs_load(struct myfs_info *fsi);
int myfs_erase(struct myfs_info *fsi);

extern const struct dentry_operations myfs_dops;
struct inode *myfs_iget(struct super_block *sb, unsigned long ino);
int myfs_find_file(const struct myfs_info *fsi, const char *name, u32 *idx);

extern const struct file_operations myfs_file_fops;
extern const struct inode_operations myfs_file_iops;

long myfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif
