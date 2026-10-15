// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>

#include "myfs.h"

// Раскладывает файлы по участкам между суперблоками и возвращает их число
u32 myfs_layout(const struct myfs_super_block *sb, struct myfs_file *files)
{
	u32 lo = min(sb->sb_offset_1, sb->sb_offset_2);
	u32 hi = max(sb->sb_offset_1, sb->sb_offset_2);
	u32 area[3][2] = {
		{ 0, lo },
		{ lo + 1, hi },
		{ hi + 1, sb->total_sectors },
	};
	u32 n = 0, a, i;

	for (a = 0; a < 3; a++) {
		u32 count = (area[a][1] - area[a][0]) / sb->file_sectors;

		for (i = 0; files && i < count; i++) {
			struct myfs_file *f = &files[n + i];

			snprintf(f->name, sizeof(f->name), MYFS_NAME_FMT, n + i);
			f->start = area[a][0] + i * sb->file_sectors;
			f->sectors = sb->file_sectors;
		}
		n += count;
	}
	return n;
}

// Проверяет, что суперблок подходит к этому диску
int myfs_check_geometry(const struct myfs_info *fsi,
			const struct myfs_super_block *sb)
{
	u64 dev_sectors = bdev_nr_bytes(fsi->bdev) >> blksize_bits(fsi->sector_size);
	u64 file_bytes = (u64)sb->file_sectors * fsi->sector_size;
	unsigned int name_len;

	if (sb->sector_size != fsi->sector_size) {
		pr_err("ФС создана для секторов по %u байт, у устройства %u\n",
		       sb->sector_size, fsi->sector_size);
		return -EINVAL;
	}
	if (sb->total_sectors > dev_sectors) {
		pr_err("в ФС %u секторов, на устройстве только %llu\n",
		       sb->total_sectors, dev_sectors);
		return -EINVAL;
	}
	if (sb->sb_offset_1 == sb->sb_offset_2 ||
	    sb->sb_offset_1 >= sb->total_sectors ||
	    sb->sb_offset_2 >= sb->total_sectors) {
		pr_err("секторы суперблоков %u и %u должны различаться и быть меньше %u\n",
		       sb->sb_offset_1, sb->sb_offset_2, sb->total_sectors);
		return -EINVAL;
	}
	if (!sb->file_sectors || file_bytes > MYFS_MAX_FILE_BYTES) {
		pr_err("файл из %u секторов по %u байт, допустимо от 1 сектора до %lu байт\n",
		       sb->file_sectors, fsi->sector_size, MYFS_MAX_FILE_BYTES);
		return -EINVAL;
	}
	if (!sb->num_files || sb->num_files != myfs_layout(sb, NULL)) {
		pr_err("число файлов %u не совпадает с раскладкой устройства\n",
		       sb->num_files);
		return -EINVAL;
	}
	name_len = snprintf(NULL, 0, MYFS_NAME_FMT, sb->num_files - 1);
	if (sb->max_name_len > MYFS_NAME_MAX || name_len > sb->max_name_len) {
		pr_err("именам файлов нужно %u символов, а max_name_len = %u\n",
		       name_len, sb->max_name_len);
		return -EINVAL;
	}
	return 0;
}
