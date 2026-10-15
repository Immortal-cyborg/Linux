// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "myfs.h"

// Номер файла по номеру inode
static u32 myfs_index(const struct inode *inode)
{
	return inode->i_ino - MYFS_FIRST_FILE_INO;
}

// Чтение: берём файл с диска целиком и отдаём нужную часть
static ssize_t myfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct myfs_info *fsi = MYFS_INFO(inode->i_sb);
	loff_t size = i_size_read(inode);
	loff_t pos = iocb->ki_pos;
	size_t count;
	char *buf;
	int ret;

	if (pos >= size || !iov_iter_count(to))
		return 0;
	count = min_t(size_t, iov_iter_count(to), size - pos);

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mutex_lock(&fsi->lock);
	ret = fsi->erased ? -EIO :
	      myfs_file_rw(fsi, myfs_index(inode), buf, REQ_OP_READ);
	mutex_unlock(&fsi->lock);
	if (!ret) {
		count = copy_to_iter(buf + pos, count, to);
		iocb->ki_pos += count;
	}
	kfree(buf);
	if (ret)
		return ret;
	return count ? count : -EFAULT;
}

// Запись: читаем файл, накладываем новые байты и пишем его обратно
static ssize_t myfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct myfs_info *fsi = MYFS_INFO(inode->i_sb);
	size_t size = i_size_read(inode);
	u32 idx = myfs_index(inode);
	ssize_t count;
	char *buf;
	int ret = 0;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	inode_lock(inode);
	count = generic_write_checks(iocb, from);
	if (count <= 0)
		goto out_unlock;

	mutex_lock(&fsi->lock);
	if (fsi->erased)
		ret = -EIO;
	else
		ret = myfs_file_rw(fsi, idx, buf, REQ_OP_READ);
	if (!ret) {
		count = copy_from_iter(buf + iocb->ki_pos, count, from);
		ret = count ? myfs_file_rw(fsi, idx, buf, REQ_OP_WRITE) : -EFAULT;
	}
	mutex_unlock(&fsi->lock);

	if (!ret) {
		iocb->ki_pos += count;
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	}
out_unlock:
	inode_unlock(inode);
	kfree(buf);
	return ret ? ret : count;
}

// Обрезка: обнуляет всё после новой длины, размер файла не меняется
static int myfs_truncate(struct inode *inode, loff_t newsize)
{
	struct myfs_info *fsi = MYFS_INFO(inode->i_sb);
	size_t size = i_size_read(inode);
	u32 idx = myfs_index(inode);
	char *buf;
	int ret = 0;

	if (newsize >= size)
		return 0;
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mutex_lock(&fsi->lock);
	if (fsi->erased)
		ret = -EIO;
	else if (newsize)
		ret = myfs_file_rw(fsi, idx, buf, REQ_OP_READ);
	if (!ret) {
		memset(buf + newsize, 0, size - newsize);
		ret = myfs_file_rw(fsi, idx, buf, REQ_OP_WRITE);
	}
	mutex_unlock(&fsi->lock);
	kfree(buf);
	return ret;
}

// Права и владельца менять нельзя, смена размера идёт через обрезку
static int myfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	if (attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID))
		return -EPERM;
	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;
	if (attr->ia_valid & ATTR_SIZE) {
		ret = myfs_truncate(inode, attr->ia_size);
		if (ret)
			return ret;
		attr->ia_valid &= ~ATTR_SIZE;
	}
	setattr_copy(idmap, inode, attr);
	return 0;
}

const struct file_operations myfs_file_fops = {
	.open		= generic_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= myfs_read_iter,
	.write_iter	= myfs_write_iter,
	.unlocked_ioctl	= myfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

const struct inode_operations myfs_file_iops = {
	.setattr	= myfs_setattr,
};
