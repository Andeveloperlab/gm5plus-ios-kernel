#!/bin/bash
# Build the GM 5 Plus (shamrock - MSM8952) boot shim.
# Wraps into a standard Qualcomm v0 boot.img with shamrock.dtb.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
READELF=aarch64-linux-gnu-readelf
NM=aarch64-linux-gnu-nm
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out

OBJS="out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ci_usb.o out/shamrock_charger.o"

for f in start.S shim.c console.c mmu.c fdt.c loader.c ci_usb.c shamrock_charger.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done

# Verify no absolute relocations in C objects
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ci_usb.o out/shamrock_charger.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,-N \
	$OBJS -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image

# Pad to image_size so .bss and stacks are zeroed bytes
hex_end=$($NM out/shim.elf | awk '$3=="_end"{print $1}')
size=$(python3 -c "import sys; print(int(sys.argv[1], 16))" "$hex_end")
truncate -s "$size" out/Image

# Set Qualcomm LK DTB_OFFSET at 0x2C in the ARM64 Image header
python3 -c "import struct; f = open('out/Image', 'r+b'); f.seek(0x2c); f.write(struct.pack('<I', $size)); f.close()"

# Prepare ramdisk (either payload or empty cpio)
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi

DTB="../out/shamrock.dtb"
[ -f "$DTB" ] || { python3 ../tools/gen-shamrock-dtb.py "$DTB"; }

# Append DTB to ARM64 kernel Image (required by LK/lk2nd)
cat out/Image "$DTB" > out/Image.dtb

# MSM8952 standard boot.img arguments (base 0x80000000, 2048 page size)
mkbootimg \
	--header_version 0 \
	--kernel out/Image.dtb \
	--ramdisk out/ramdisk.bin \
	--base 0x80000000 \
	--kernel_offset 0x00080000 \
	--ramdisk_offset 0x01000000 \
	--tags_offset 0x00000100 \
	--pagesize 2048 \
	--cmdline "console=ttyHSL0,115200,n8 androidboot.hardware=qcom" \
	-o out/shamrock-shim-boot.img

cp out/shamrock-shim-boot.img ../out/shamrock-shim-boot.img

echo "[+] shamrock-shim built successfully:"
ls -lh out/Image out/shamrock-shim-boot.img
