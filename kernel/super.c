// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "myfs.h"

static u32 myfs_sb_csum(const struct myfs_super_block *sb)
{
	return myfs_crc32(sb, offsetof(struct myfs_super_block, checksum));
}

static int myfs_read_sb(struct myfs_info *fsi, u32 sector,
			struct myfs_super_block *sb)
{
	void *buf = kmalloc(fsi->sector_size, GFP_KERNEL);
	int ret;

	if (!buf)
		return -ENOMEM;
	ret = myfs_rw(fsi, sector, buf, fsi->sector_size, REQ_OP_READ);
	if (!ret)
		memcpy(sb, buf, sizeof(*sb));
	kfree(buf);
	return ret;
}

static int myfs_write_sb(struct myfs_info *fsi, u32 sector,
			 const struct myfs_super_block *sb)
{
	void *buf = kzalloc(fsi->sector_size, GFP_KERNEL);
	int ret;

	if (!buf)
		return -ENOMEM;
	if (sb)
		memcpy(buf, sb, sizeof(*sb));
	ret = myfs_rw(fsi, sector, buf, fsi->sector_size,
		      REQ_OP_WRITE | REQ_PREFLUSH | REQ_FUA);
	kfree(buf);
	return ret;
}

static bool myfs_sb_valid(const struct myfs_info *fsi,
			  const struct myfs_super_block *sb)
{
	if (sb->magic != MYFS_MAGIC || sb->checksum != myfs_sb_csum(sb))
		return false;
	if (sb->version != MYFS_VERSION) {
		pr_err("unsupported format version %u\n", sb->version);
		return false;
	}
	if (sb->sb_offset_1 != sb_offset_1 || sb->sb_offset_2 != sb_offset_2) {
		pr_err("superblocks are at sectors %u and %u, load the module with these sb_offset_1/sb_offset_2\n",
		       sb->sb_offset_1, sb->sb_offset_2);
		return false;
	}
	return !myfs_check_geometry(fsi, sb);
}

static int myfs_build_files(struct myfs_info *fsi)
{
	fsi->files = kvcalloc(fsi->sb.num_files, sizeof(*fsi->files), GFP_KERNEL);
	if (!fsi->files)
		return -ENOMEM;
	myfs_layout(&fsi->sb, fsi->files);
	return 0;
}

int myfs_load(struct myfs_info *fsi)
{
	struct myfs_super_block *copy;
	bool ok1, ok2;
	int ret = 0;

	copy = kmalloc_array(2, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	ok1 = !myfs_read_sb(fsi, sb_offset_1, &copy[0]) && myfs_sb_valid(fsi, &copy[0]);
	ok2 = !myfs_read_sb(fsi, sb_offset_2, &copy[1]) && myfs_sb_valid(fsi, &copy[1]);
	if (!ok1 && !ok2) {
		pr_err("both superblocks are invalid, refusing to mount (create a filesystem with format=1)\n");
		ret = -EINVAL;
		goto out;
	}

	fsi->sb = ok1 ? copy[0] : copy[1];
	if (!ok1 || !ok2 || memcmp(&copy[0], &copy[1], sizeof(*copy))) {
		u32 bad = ok1 ? sb_offset_2 : sb_offset_1;

		pr_warn("superblock at sector %u is damaged, restoring it\n", bad);
		if (myfs_write_sb(fsi, bad, &fsi->sb))
			pr_warn("could not restore superblock at sector %u\n", bad);
	}
	ret = myfs_build_files(fsi);
out:
	kfree(copy);
	return ret;
}

int myfs_format(struct myfs_info *fsi)
{
	struct myfs_super_block *sb = &fsi->sb;
	u64 dev_sectors = bdev_nr_bytes(fsi->bdev) >> blksize_bits(fsi->sector_size);
	int ret;

	if (dev_sectors > U32_MAX) {
		pr_err("device has %llu sectors, at most %u supported\n",
		       dev_sectors, U32_MAX);
		return -EFBIG;
	}

	memset(sb, 0, sizeof(*sb));
	sb->magic = MYFS_MAGIC;
	sb->version = MYFS_VERSION;
	sb->sector_size = fsi->sector_size;
	sb->total_sectors = dev_sectors;
	sb->sb_offset_1 = sb_offset_1;
	sb->sb_offset_2 = sb_offset_2;
	sb->max_name_len = max_name_len;
	sb->file_sectors = file_sectors;
	sb->num_files = myfs_layout(sb, NULL);
	sb->checksum = myfs_sb_csum(sb);

	ret = myfs_check_geometry(fsi, sb);
	if (ret)
		return ret;
	ret = myfs_build_files(fsi);
	if (ret)
		return ret;

	ret = myfs_write_sb(fsi, sb_offset_1, NULL);
	if (!ret)
		ret = myfs_write_sb(fsi, sb_offset_2, NULL);
	if (!ret)
		ret = myfs_zero_files(fsi);
	if (!ret)
		ret = myfs_write_sb(fsi, sb_offset_1, sb);
	if (!ret)
		ret = myfs_write_sb(fsi, sb_offset_2, sb);
	if (ret) {
		pr_err("format of /dev/%s failed: %d\n", myfs_dev, ret);
		return ret;
	}

	pr_info("created filesystem on /dev/%s: %u files of %u sectors, sector size %u\n",
		myfs_dev, sb->num_files, sb->file_sectors, fsi->sector_size);
	return 0;
}

int myfs_erase(struct myfs_info *fsi)
{
	int ret = myfs_write_sb(fsi, fsi->sb.sb_offset_1, NULL);

	if (!ret)
		ret = myfs_write_sb(fsi, fsi->sb.sb_offset_2, NULL);
	if (ret)
		return ret;
	WRITE_ONCE(fsi->erased, true);
	pr_info("filesystem on /dev/%s erased\n", myfs_dev);
	return 0;
}
