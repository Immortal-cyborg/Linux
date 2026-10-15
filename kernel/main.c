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
MODULE_PARM_DESC(myfs_dev, "Имя блочного устройства без /dev/, например loop0 (по умолчанию sdb)");
module_param(sb_offset_1, uint, 0444);
MODULE_PARM_DESC(sb_offset_1, "Сектор основного суперблока (по умолчанию 0)");
module_param(sb_offset_2, uint, 0444);
MODULE_PARM_DESC(sb_offset_2, "Сектор копии суперблока (по умолчанию 1)");
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Максимальная длина имени файла, 9..63 (по умолчанию 63)");
module_param(file_sectors, uint, 0444);
MODULE_PARM_DESC(file_sectors, "Размер файла M в секторах, не больше 1 МиБ (по умолчанию 8)");
module_param(format, bool, 0444);
MODULE_PARM_DESC(format, "Создать новую ФС на устройстве при загрузке модуля (по умолчанию 0)");

// Открывает диск из параметра myfs_dev в монопольном режиме
static int myfs_open_dev(struct myfs_info *fsi, void *holder)
{
	char path[sizeof("/dev/") + DISK_NAME_LEN];
	struct file *f;

	snprintf(path, sizeof(path), "/dev/%s", myfs_dev);
	f = bdev_file_open_by_path(path, BLK_OPEN_READ | BLK_OPEN_WRITE,
				   holder, NULL);
	if (IS_ERR(f)) {
		pr_err("не удалось открыть %s: %ld\n", path, PTR_ERR(f));
		return PTR_ERR(f);
	}
	fsi->bdev_file = f;
	fsi->bdev = file_bdev(f);
	fsi->sector_size = bdev_logical_block_size(fsi->bdev);
	return 0;
}

// Закрывает диск и освобождает память ФС
static void myfs_free_info(struct myfs_info *fsi)
{
	if (fsi->bdev_file)
		bdev_fput(fsi->bdev_file);
	kvfree(fsi->files);
	kfree(fsi);
}

static const struct super_operations myfs_sops = {
	.statfs	    = simple_statfs,
};

// Монтирование: читает суперблок и создаёт корневой каталог
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

	pr_info("смонтирована /dev/%s: %u файлов по %u секторов, размер сектора %u\n",
		myfs_dev, fsi->sb.num_files, fsi->sb.file_sectors, fsi->sector_size);
	return 0;
}

// Вход в mount: новый суперблок, диск берётся не из командной строки
static int myfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, myfs_fill_super);
}

static const struct fs_context_operations myfs_context_ops = {
	.get_tree	= myfs_get_tree,
};

// Подключает операции монтирования
static int myfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &myfs_context_ops;
	return 0;
}

// Размонтирование: уборка VFS, затем данных
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

// Проверяет параметры модуля, которые не зависят от диска
static int __init myfs_check_params(void)
{
	unsigned int min_name_len = snprintf(NULL, 0, MYFS_NAME_FMT, 0);

	if (!*myfs_dev || strchr(myfs_dev, '/') ||
	    strlen(myfs_dev) >= DISK_NAME_LEN) {
		pr_err("myfs_dev должен быть именем устройства, например loop0 (без '/', короче %d символов)\n",
		       DISK_NAME_LEN);
		return -EINVAL;
	}
	if (sb_offset_1 == sb_offset_2) {
		pr_err("sb_offset_1 и sb_offset_2 должны различаться\n");
		return -EINVAL;
	}
	if (max_name_len < min_name_len || max_name_len > MYFS_NAME_MAX) {
		pr_err("max_name_len должен быть в диапазоне %u..%d\n", min_name_len, MYFS_NAME_MAX);
		return -EINVAL;
	}
	if (!file_sectors || file_sectors > MYFS_MAX_FILE_BYTES / SECTOR_SIZE) {
		pr_err("file_sectors должен быть в диапазоне 1..%lu\n",
		       MYFS_MAX_FILE_BYTES / SECTOR_SIZE);
		return -EINVAL;
	}
	return 0;
}

// Форматирует диск при insmod с format=1
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

// insmod: проверка параметров, форматирование и регистрация ФС
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

// rmmod: снимает регистрацию ФС
static void __exit myfs_exit(void)
{
	unregister_filesystem(&myfs_type);
}

module_init(myfs_init);
module_exit(myfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Учебная файловая система поверх блочного устройства");
