#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock header.
# Works on Linux (aarch64-linux-gnu-*) and macOS/Homebrew (aarch64-elf-*).
# Env overrides: CROSS (tool prefix), DTB (stock davinci DTB), MKBOOTIMG,
# EXTRA_CFLAGS (e.g. -DDAVINCI_DIAG_ONLY for the framebuffer/UFS probe image).
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${CROSS:-}" ]; then
	if command -v aarch64-elf-gcc >/dev/null; then CROSS=aarch64-elf-
	else CROSS=aarch64-linux-gnu-; fi
fi
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
READELF=${CROSS}readelf
NM=${CROSS}nm
TRUNCATE=$(command -v gtruncate || command -v truncate)
if command -v brew >/dev/null && [ -x "$(brew --prefix cpio 2>/dev/null)/bin/cpio" ]; then
	CPIO="$(brew --prefix cpio)/bin/cpio"
else
	CPIO=cpio
fi
DTB=${DTB:-../dtb/davinci.dtb}
MKBOOTIMG=${MKBOOTIMG:-../aosp-mkbootimg/mkbootimg.py}

CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra ${EXTRA_CFLAGS:-}"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($NM out/shim.elf | gawk '$3=="_end"{print strtonum("0x"$1)}')
$TRUNCATE -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | "$CPIO" -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
if [ ! -f "$DTB" ]; then
	echo "error: stock DTB $DTB missing (python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked; cp dtb/boot_unpacked/dtb dtb/davinci.dtb)" >&2
	exit 1
fi
if [ "${BOOTFMT:-v0gz}" = v0gz ]; then
	# davinci's ABL boots what pmOS's U-Boot image looks like: header v0, kernel =
	# gzip(Image) with the DTB appended (Image.gz-dtb), no DTB section. It refuses
	# the v2 + raw Image + DTB-section layout that sweet's ABL accepted.
	gzip -9 -n -c out/Image > out/Image.gz
	cat out/Image.gz "$DTB" > out/Image.gz-dtb
	python3 "$MKBOOTIMG" --header_version 0 --kernel out/Image.gz-dtb --ramdisk out/ramdisk.bin \
		--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --pagesize 4096 \
		--cmdline "console=null" -o out/shim-boot.img
else
	python3 "$MKBOOTIMG" --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version 17.0.0 --os_patch_level 2035-06 \
		--cmdline "console=null" -o out/shim-boot.img
fi
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
