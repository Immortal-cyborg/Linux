// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/kstrtox.h>

#include "myfs.h"

int myfs_find_file(const struct myfs_info *fsi, const char *name, u32 *idx)
{
	const size_t plen = sizeof(MYFS_NAME_PREFIX) - 1;

	if (strncmp(name, MYFS_NAME_PREFIX, plen) ||
	    kstrtou32(name + plen, 10, idx) || *idx >= fsi->sb.num_files ||
	    strcmp(fsi->files[*idx].name, name))
		return -ENOENT;
	return 0;
}

static int myfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct myfs_info *fsi = MYFS_INFO(file_inode(file)->i_sb);

	if (!dir_emit_dots(file, ctx))
		return 0;
	if (READ_ONCE(fsi->erased))
		return 0;
	for (; ctx->pos - 2 < fsi->sb.num_files; ctx->pos++) {
		u32 idx = ctx->pos - 2;
		const char *name = fsi->files[idx].name;

		if (!dir_emit(ctx, name, strlen(name), idx + MYFS_FIRST_FILE_INO,
			      DT_REG))
			return 0;
	}
	return 0;
}

static loff_t myfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct myfs_info *fsi = MYFS_INFO(file_inode(file)->i_sb);
	loff_t end = (loff_t)fsi->sb.num_files + 2;

	return generic_file_llseek_size(file, offset, whence, end, end);
}

static struct dentry *myfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct myfs_info *fsi = MYFS_INFO(dir->i_sb);
	struct inode *inode = NULL;
	u32 idx;

	if (dentry->d_name.len > fsi->sb.max_name_len)
		return ERR_PTR(-ENAMETOOLONG);
	if (!READ_ONCE(fsi->erased) &&
	    !myfs_find_file(fsi, dentry->d_name.name, &idx)) {
		inode = myfs_iget(dir->i_sb, idx + MYFS_FIRST_FILE_INO);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}
	return d_splice_alias(inode, dentry);
}

static int myfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	return IS_ROOT(dentry) || !READ_ONCE(MYFS_INFO(dentry->d_sb)->erased);
}

const struct dentry_operations myfs_dops = {
	.d_revalidate   = myfs_d_revalidate,
};

static const struct file_operations myfs_dir_fops = {
	.llseek	        = myfs_dir_llseek,
	.read           = generic_read_dir,
	.iterate_shared	= myfs_readdir,
	.unlocked_ioctl	= myfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct inode_operations myfs_dir_iops = {
	.lookup		    = myfs_lookup,
};

struct inode *myfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode = iget_locked(sb, ino);

	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	simple_inode_init_ts(inode);
	if (ino == MYFS_ROOT_INO) {
		inode->i_mode   = S_IFDIR | 0755;
		inode->i_op     = &myfs_dir_iops;
		inode->i_fop    = &myfs_dir_fops;
		set_nlink(inode, 2);
	} else {
		inode->i_mode   = S_IFREG | 0666;
		inode->i_op     = &myfs_file_iops;
		inode->i_fop    = &myfs_file_fops;
		inode->i_size   = myfs_file_bytes(MYFS_INFO(sb));
		inode->i_blocks = inode->i_size >> SECTOR_SHIFT;
	}
	unlock_new_inode(inode);
	return inode;
}
