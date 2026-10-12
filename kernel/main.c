// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "myfs.h"

char *myfs_dev = "sdb";
uint sb_offset_1;
uint sb_offset_2 = 1;
uint max_name_len = MYFS_NAME_MAX;
uint file_sectors = 8;
static bool format;

module_param(myfs_dev, charp, 0444);
MODULE_PARM_DESC(myfs_dev, "Block device name without /dev/, e.g. loop0 (default sdb)");
module_param(sb_offset_1, uint, 0444);
MODULE_PARM_DESC(sb_offset_1, "Sector of the primary superblock (default 0)");
module_param(sb_offset_2, uint, 0444);
MODULE_PARM_DESC(sb_offset_2, "Sector of the backup superblock (default 1)");
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Max file name length, 9..63 (default 63)");
module_param(file_sectors, uint, 0444);
MODULE_PARM_DESC(file_sectors, "File size M in sectors, at most 1 MiB per file (default 8)");
module_param(format, bool, 0444);
MODULE_PARM_DESC(format, "Create a new filesystem on the device at module load (default 0)");

static int myfs_open_dev(struct myfs_info *fsi, void *holder)
{
	char path[sizeof("/dev/") + DISK_NAME_LEN];
	struct file *f;

	snprintf(path, sizeof(path), "/dev/%s", myfs_dev);
	f = bdev_file_open_by_path(path, BLK_OPEN_READ | BLK_OPEN_WRITE,
				   holder, NULL);
	if (IS_ERR(f)) {
		pr_err("cannot open %s: %ld\n", path, PTR_ERR(f));
		return PTR_ERR(f);
	}
	fsi->bdev_file = f;
	fsi->bdev = file_bdev(f);
	fsi->sector_size = bdev_logical_block_size(fsi->bdev);
	return 0;
}

static void myfs_free_info(struct myfs_info *fsi)
{
	if (fsi->bdev_file)
		bdev_fput(fsi->bdev_file);
	kvfree(fsi->files);
	kfree(fsi);
}

static const struct super_operations myfs_sops = {
	.statfs		= simple_statfs,
};

static int myfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct myfs_info *fsi;
	struct inode *root;
	int ret;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;
	mutex_init(&fsi->lock);
	sb->s_fs_info = fsi;

	ret = myfs_open_dev(fsi, sb);
	if (ret)
		return ret;
	ret = myfs_load(fsi);
	if (ret)
		return ret;

	sb->s_magic = MYFS_MAGIC;
	sb->s_op = &myfs_sops;
	sb->s_d_op = &myfs_dops;
	sb->s_blocksize = fsi->sector_size;
	sb->s_blocksize_bits = blksize_bits(fsi->sector_size);
	sb->s_maxbytes = myfs_file_bytes(fsi);
	sb->s_time_gran = 1;

	root = myfs_iget(sb, MYFS_ROOT_INO);
	if (IS_ERR(root))
		return PTR_ERR(root);
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;

	pr_info("mounted /dev/%s: %u files of %u sectors, sector size %u\n",
		myfs_dev, fsi->sb.num_files, fsi->sb.file_sectors, fsi->sector_size);
	return 0;
}

static int myfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, myfs_fill_super);
}

static const struct fs_context_operations myfs_context_ops = {
	.get_tree	= myfs_get_tree,
};

static int myfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &myfs_context_ops;
	return 0;
}

static void myfs_kill_sb(struct super_block *sb)
{
	struct myfs_info *fsi = MYFS_INFO(sb);

	kill_anon_super(sb);
	if (fsi)
		myfs_free_info(fsi);
}

static struct file_system_type myfs_type = {
	.owner			= THIS_MODULE,
	.name			= "myfs",
	.init_fs_context	= myfs_init_fs_context,
	.kill_sb		= myfs_kill_sb,
};
MODULE_ALIAS_FS("myfs");

static int __init myfs_check_params(void)
{
	unsigned int min_name_len = snprintf(NULL, 0, MYFS_NAME_FMT, 0);

	if (!*myfs_dev || strchr(myfs_dev, '/') ||
	    strlen(myfs_dev) >= DISK_NAME_LEN) {
		pr_err("myfs_dev must be a device name like loop0 (no '/', < %d chars)\n",
		       DISK_NAME_LEN);
		return -EINVAL;
	}
	if (sb_offset_1 == sb_offset_2) {
		pr_err("sb_offset_1 and sb_offset_2 must differ\n");
		return -EINVAL;
	}
	if (max_name_len < min_name_len || max_name_len > MYFS_NAME_MAX) {
		pr_err("max_name_len must be in %u..%d\n", min_name_len, MYFS_NAME_MAX);
		return -EINVAL;
	}
	if (!file_sectors || file_sectors > MYFS_MAX_FILE_BYTES / SECTOR_SIZE) {
		pr_err("file_sectors must be in 1..%lu\n",
		       MYFS_MAX_FILE_BYTES / SECTOR_SIZE);
		return -EINVAL;
	}
	return 0;
}

static int __init myfs_format_dev(void)
{
	struct myfs_info *fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	int ret;

	if (!fsi)
		return -ENOMEM;
	ret = myfs_open_dev(fsi, fsi);
	if (!ret)
		ret = myfs_format(fsi);
	myfs_free_info(fsi);
	return ret;
}

static int __init myfs_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct myfs_super_block) != MYFS_SB_SIZE);

	ret = myfs_check_params();
	if (ret)
		return ret;
	if (format) {
		ret = myfs_format_dev();
		if (ret)
			return ret;
	}
	return register_filesystem(&myfs_type);
}

static void __exit myfs_exit(void)
{
	unregister_filesystem(&myfs_type);
}

module_init(myfs_init);
module_exit(myfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Educational filesystem on top of a block device");
