# MyFS

Файловая система для Linux 6.12 в виде модуля ядра. Модуль размечает
блочное устройство на файлы одинакового размера; ФС монтируется в `/mnt`,
файлы читаются и записываются обычными средствами. Утилита `myfs_tool`
выполняет тест записи и чтения и вызывает команды ioctl.

## Раскладка диска

Пример для `sb_offset_2=100` и файлов по 4 сектора:

```
0          суперблок
1..96      file00000 … file00023
97..99     не используются (меньше размера файла)
100        копия суперблока
101..      file00024, file00025, …
```

Каждый файл занимает непрерывный участок и читается или записывается
одним bio. Обе копии суперблока защищены CRC-32. При монтировании
испорченная копия восстанавливается из исправной; если испорчены обе,
монтирование завершается ошибкой.

## Параметры модуля

```
myfs_dev       имя устройства без /dev/, например loop0   (sdb)
sb_offset_1    сектор основного суперблока                (0)
sb_offset_2    сектор копии суперблока                    (1)
max_name_len   максимальная длина имени, 9..63            (63)
file_sectors   размер файла в секторах, не больше 1 МиБ   (8)
format         1 — создать ФС при загрузке модуля         (0)
```

ФС создаётся только при загрузке модуля с `format=1`, монтирование её не
форматирует.

## Возможности

Файлы называются `file00000`, `file00001` и т. д. Поддерживаются чтение и
запись в пределах файла. `>` обнуляет содержимое после записанных данных,
`>>` завершается ошибкой, так как размер файла фиксирован. Создание,
удаление и переименование файлов не поддерживаются.

Команды ioctl вызываются на `/mnt` или на любом файле ФС:

- `MYFS_IOC_ZERO_ALL` — обнулить все файлы;
- `MYFS_IOC_ERASE_FS` — стереть обе копии суперблока, после чего ФС не
  монтируется;
- `MYFS_IOC_GET_META` — CRC-32 содержимого каждого файла;
- `MYFS_IOC_GET_SECTOR_MAP` — расположение файла на диске по его имени.

## Сборка и запуск

```bash
sudo apt install build-essential linux-headers-$(uname -r)
make

dd if=/dev/zero of=/tmp/myfs.img bs=1M count=4
sudo losetup /dev/loop0 /tmp/myfs.img
sudo insmod kernel/myfs.ko myfs_dev=loop0 sb_offset_2=100 file_sectors=4 format=1
sudo mount -t myfs none /mnt

sudo userspace/myfs_tool test
sudo userspace/myfs_tool metadata
sudo userspace/myfs_tool sectormap file00024
sudo userspace/myfs_tool zero
sudo userspace/myfs_tool erase

sudo umount /mnt
sudo rmmod myfs
sudo losetup -d /dev/loop0
```

## Проверка

```bash
sudo make check
sudo SECTOR=4096 sh tests/selftest.sh
```