// SPDX-License-Identifier: GPL-2.0
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/slab.h>

#include "myfs.h"

// Читает или пишет len байт начиная с сектора sector одним bio и ждёт диск
int myfs_rw(struct myfs_info *fsi, u32 sector, void *buf, size_t len,
	    blk_opf_t opf)
{
	unsigned int nr_pages = DIV_ROUND_UP(offset_in_page(buf) + len, PAGE_SIZE);
	struct bio *bio;
	char *p = buf;
	int ret;

	if (WARN_ON_ONCE(nr_pages > BIO_MAX_VECS))
		return -EINVAL;

	bio = bio_alloc(fsi->bdev, nr_pages, opf, GFP_KERNEL);
	bio->bi_iter.bi_sector = (sector_t)sector <<
		(blksize_bits(fsi->sector_size) - SECTOR_SHIFT);
	while (len) {
		size_t off = offset_in_page(p);
		size_t chunk = min_t(size_t, len, PAGE_SIZE - off);

		__bio_add_page(bio, virt_to_page(p), chunk, off);
		p += chunk;
		len -= chunk;
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

// Читает или пишет файл целиком
int myfs_file_rw(struct myfs_info *fsi, u32 idx, void *buf, blk_opf_t opf)
{
	return myfs_rw(fsi, fsi->files[idx].start, buf, myfs_file_bytes(fsi), opf);
}

// Записывает нули во все файлы
int myfs_zero_files(struct myfs_info *fsi)
{
	void *zero = kzalloc(myfs_file_bytes(fsi), GFP_KERNEL);
	int ret = 0;
	u32 i;

	if (!zero)
		return -ENOMEM;
	for (i = 0; i < fsi->sb.num_files && !ret; i++)
		ret = myfs_file_rw(fsi, i, zero, REQ_OP_WRITE);
	kfree(zero);
	return ret;
}

// Стандартный CRC-32, такой же, как zlib.crc32
u32 myfs_crc32(const void *buf, size_t len)
{
	return ~crc32_le(~0U, buf, len);
}
