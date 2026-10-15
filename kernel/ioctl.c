// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "myfs.h"

// GET_META: CRC-32 каждого файла в массив программы
static int myfs_get_meta(struct myfs_info *fsi, struct myfs_meta_req __user *ureq)
{
	struct myfs_file_meta __user *out;
	struct myfs_file_meta meta;
	struct myfs_meta_req req;
	size_t size = myfs_file_bytes(fsi);
	int ret = 0;
	char *buf;
	u32 i, n;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;
	out = u64_to_user_ptr(req.entries);
	n = min(req.count, fsi->sb.num_files);
	if (!n)
		return put_user(fsi->sb.num_files, &ureq->count) ? -EFAULT : 0;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < n && !ret; i++) {
		ret = myfs_file_rw(fsi, i, buf, REQ_OP_READ);
		if (ret)
			break;
		memset(&meta, 0, sizeof(meta));
		meta.index = i;
		meta.crc32 = myfs_crc32(buf, size);
		strscpy(meta.name, fsi->files[i].name, sizeof(meta.name));
		if (copy_to_user(&out[i], &meta, sizeof(meta)))
			ret = -EFAULT;
	}
	kfree(buf);
	if (!ret && put_user(fsi->sb.num_files, &ureq->count))
		ret = -EFAULT;
	return ret;
}

// GET_SECTOR_MAP: где на диске лежит файл с данным именем
static int myfs_get_sector_map(struct myfs_info *fsi,
			       struct myfs_sector_map __user *umap)
{
	struct myfs_sector_map map;
	u32 idx;

	if (copy_from_user(&map, umap, sizeof(map)))
		return -EFAULT;
	map.name[sizeof(map.name) - 1] = '\0';
	if (myfs_find_file(fsi, map.name, &idx))
		return -ENOENT;

	map.index = idx;
	map.sector_size = fsi->sector_size;
	map.start = fsi->files[idx].start;
	map.count = fsi->files[idx].sectors;
	return copy_to_user(umap, &map, sizeof(map)) ? -EFAULT : 0;
}

// Общий обработчик ioctl: проверяет права и выполняет команду под замком
long myfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct myfs_info *fsi = MYFS_INFO(file_inode(file)->i_sb);
	void __user *uarg = (void __user *)arg;
	long ret;

	switch (cmd) {
	case MYFS_IOC_ZERO_ALL:
	case MYFS_IOC_ERASE_FS:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		break;
	case MYFS_IOC_GET_META:
	case MYFS_IOC_GET_SECTOR_MAP:
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&fsi->lock);
	if (fsi->erased)
		ret = -EIO;
	else if (cmd == MYFS_IOC_ZERO_ALL)
		ret = myfs_zero_files(fsi);
	else if (cmd == MYFS_IOC_ERASE_FS)
		ret = myfs_erase(fsi);
	else if (cmd == MYFS_IOC_GET_META)
		ret = myfs_get_meta(fsi, uarg);
	else
		ret = myfs_get_sector_map(fsi, uarg);
	mutex_unlock(&fsi->lock);
	return ret;
}
