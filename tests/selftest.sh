#!/bin/sh
set -u

DIR=$(cd "$(dirname "$0")/.." && pwd)
KO=$DIR/kernel/myfs.ko
TOOL=$DIR/userspace/myfs_tool
M=4
SB2=100
[ "$(id -u)" = 0 ] || { echo "Запуск от root: sudo sh tests/selftest.sh"; exit 1; }
[ -f "$KO" ] && [ -x "$TOOL" ] || { echo "Сначала собрать проект: make"; exit 1; }
command -v python3 >/dev/null || { echo "Нужен python3: sudo apt install python3"; exit 1; }
if grep -q '^myfs ' /proc/modules; then
	echo "Модуль myfs уже загружен"
	exit 1
fi

TMP=$(mktemp -d) || exit 1
MNT=$TMP/mnt
LOOP=
PASS=0
FAIL=0

cleanup() {
	umount "$MNT" 2>/dev/null
	umount "$TMP/mnt2" 2>/dev/null
	rmmod myfs 2>/dev/null
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$TMP"
}
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); echo "  [ок]      $1"; }
fail() {
	FAIL=$((FAIL + 1))
	echo "  [ошибка]  $1"
	[ -s "$TMP/out" ] && sed 's/^/            /' "$TMP/out"
}

check() {
	desc=$1
	shift
	if "$@" >"$TMP/out" 2>&1; then ok "$desc"; else fail "$desc"; fi
}

refuse() {
	desc=$1
	shift
	if "$@" >"$TMP/out" 2>&1; then fail "$desc — но команда прошла успешно"; else ok "$desc"; fi
}

load() { insmod "$KO" myfs_dev="$NAME" sb_offset_2=$SB2 file_sectors=$M "$@"; }
fsmount() { mount -t myfs none "$MNT"; }
raw() { dd if="/dev/$NAME" bs="$SS" skip="$1" count="${2:-1}" 2>/dev/null; }
bytes() { head -c "$2" "$1" | od -An -c | tr -d ' \n'; }

insmod_refused() {
	if insmod "$KO" myfs_dev="$NAME" "$@"; then
		rmmod myfs
		return 1
	fi
}

mkdir -p "$MNT" "$TMP/mnt2"

dd if=/dev/zero of="$TMP/disk.img" bs=1M count=4 2>/dev/null || exit 1
LOOP=$(losetup --find --show --sector-size "${SECTOR:-512}" "$TMP/disk.img") || exit 1
NAME=${LOOP#/dev/}
SS=$(cat "/sys/block/$NAME/queue/logical_block_size")
TOTAL=$(($(cat "/sys/block/$NAME/size") * 512 / SS))
NFILES=$(((SB2 - 1) / M + (TOTAL - SB2 - 1) / M))
echo "Тестовый диск /dev/$NAME: $TOTAL секторов по $SS байт, ожидается $NFILES файлов"

echo
echo "Параметры модуля"
check "Файл нулевого размера (file_sectors=0) не принимается" insmod_refused file_sectors=0
check "Файл больше 1 МиБ (file_sectors=4096) не принимается" insmod_refused file_sectors=4096
check "Слишком короткое имя файла (max_name_len=5) не принимается" insmod_refused max_name_len=5
check "Обе копии суперблока в одном секторе — не принимается" insmod_refused sb_offset_1=3 sb_offset_2=3
check "Путь /dev/… вместо имени диска не принимается" insmod_refused myfs_dev="/dev/$NAME"
check "Суперблок за концом диска не принимается" insmod_refused sb_offset_2="$TOTAL" format=1

echo
echo "Создание и монтирование"
check "Модуль с format=1 создаёт файловую систему" load format=1
check "Файловая система монтируется" fsmount

t_count() { [ "$(ls "$MNT" | wc -l)" -eq "$NFILES" ]; }
check "В каталоге ровно $NFILES файлов" t_count

refuse "Тот же диск нельзя смонтировать второй раз" mount -t myfs none "$TMP/mnt2"
check "В каждый файл записано случайное число и прочитано обратно" "$TOOL" test "$MNT"

echo
echo "Чтение и запись"
t_persist() {
	printf hello > "$MNT/file00007" &&
	umount "$MNT" && fsmount &&
	[ "$(head -c 5 "$MNT/file00007")" = hello ]
}
check "Данные сохраняются после перемонтирования" t_persist

t_reload() {
	umount "$MNT" && rmmod myfs && load && fsmount &&
	[ "$(head -c 5 "$MNT/file00007")" = hello ]
}
check "Данные сохраняются после перезагрузки модуля" t_reload

t_append() { printf x >> "$MNT/file00001"; }
refuse "Дописать в конец файла (>>) нельзя: размер фиксирован" t_append

t_trunc() {
	printf hello > "$MNT/file00002" && printf hi > "$MNT/file00002" &&
	[ "$(bytes "$MNT/file00002" 5)" = 'hi\0\0\0' ] &&
	[ "$(stat -c %s "$MNT/file00002")" -eq $((M * SS)) ]
}
check "Перезапись через '>' обнуляет остаток, размер файла прежний" t_trunc

t_names() {
	! cat "$MNT/file7" 2>/dev/null &&
	ls "$MNT/$(printf 'x%.0s' $(seq 70))" 2>&1 | grep -qi 'too long'
}
check "Чужое имя — «нет такого файла», слишком длинное — «имя слишком длинное»" t_names

echo
echo "Команды ioctl"
t_map() {
	f=$(printf 'file%05d' $(((SB2 - 1) / M)))
	dd if=/dev/urandom of="$TMP/pattern" bs="$SS" count=$M 2>/dev/null &&
	dd if="$TMP/pattern" of="$MNT/$f" bs=$((SS * M)) count=1 conv=notrunc 2>/dev/null &&
	"$TOOL" sectormap "$f" "$MNT" > "$TMP/map" || return 1
	start=$(awk '$1 == "sectors" { print $2 }' "$TMP/map")
	start=${start%-*}
	[ "$start" -eq $((SB2 + 1)) ] &&
	umount "$MNT" && raw "$start" $M > "$TMP/raw" &&
	fsmount && cmp "$TMP/pattern" "$TMP/raw"
}
check "Карта секторов совпадает с тем, что лежит на диске" t_map

t_meta() {
	"$TOOL" metadata "$MNT" > "$TMP/meta" &&
	[ "$(awk 'NR > 1' "$TMP/meta" | wc -l)" -eq "$NFILES" ]
}
check "Метаданные выдаются для всех $NFILES файлов" t_meta

t_crc() {
	want=$(awk '$2 == "file00024" { print $3 }' "$TMP/meta")
	got=$(python3 -c 'import sys, zlib; print("0x%08x" % zlib.crc32(open(sys.argv[1], "rb").read()))' "$MNT/file00024")
	[ "$want" = "$got" ]
}
check "Хэш CRC-32 из ядра совпадает с посчитанным в Python" t_crc

t_zero() {
	"$TOOL" zero "$MNT" && "$TOOL" metadata "$MNT" > "$TMP/meta" &&
	[ "$(awk 'NR > 1 { print $3 }' "$TMP/meta" | sort -u | wc -l)" -eq 1 ] &&
	[ "$(bytes "$MNT/file00007" 5)" = '\0\0\0\0\0' ]
}
check "Команда zero обнуляет все файлы" t_zero

echo
echo "Суперблоки"
t_backup() {
	umount "$MNT" &&
	dd if=/dev/zero of="/dev/$NAME" bs="$SS" count=1 conv=notrunc 2>/dev/null &&
	fsmount && umount "$MNT" &&
	[ "$(raw 0 | head -c 4)" = MYFS ] && [ "$(raw $SB2 | head -c 4)" = MYFS ] &&
	fsmount
}
check "Испорченная основная копия суперблока восстанавливается из резервной" t_backup

t_erase() {
	exec 3< "$MNT/file00005" || return 1
	"$TOOL" erase -y "$MNT" &&
	[ "$(ls "$MNT" | wc -l)" -eq 0 ] &&
	! stat "$MNT/file00005" >/dev/null 2>&1 &&
	! cat <&3 >/dev/null 2>&1
	r=$?
	exec 3<&-
	[ $r -eq 0 ] && umount "$MNT"
}
check "После erase файлы сразу пропадают, открытый файл выдаёт ошибку" t_erase

refuse "После erase файловая система не монтируется" fsmount

t_reformat() { rmmod myfs && load format=1 && fsmount && t_count; }
check "После format=1 файловая система снова работает" t_reformat

echo
if [ "$FAIL" -eq 0 ]; then
	echo "Все проверки пройдены: $PASS из $PASS"
else
	echo "Пройдено $PASS из $((PASS + FAIL)), с ошибкой: $FAIL"
fi
[ "$FAIL" -eq 0 ]
