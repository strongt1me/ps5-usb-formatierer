#!/usr/bin/env bash
# Baut mkntfs (ntfs-3g 2026.9.28) und mke2fs (e2fsprogs 1.47.4) für die PS5 nach tools/bin/.
# Aufruf in WSL/Linux: PS5_PAYLOAD_SDK=/pfad/zum/ps5-payload-sdk tools/build_fstools.sh
# Muster wie in pacbrew-repo: prospero.sh einbinden, ./configure --host=x86_64-pc-freebsd.
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK setzen}"
OUT="$(cd "$(dirname "$0")" && pwd)/bin"
WORK="${WORK:-$HOME/ps5ports/src}"
mkdir -p "$OUT" "$WORK"
cd "$WORK"

fetch() {  # fetch <datei> <url> <sha256>
    [ -s "$1" ] || curl -fsSL -o "$1" "$2"
    echo "$3  $1" | sha256sum -c -
}
fetch ntfs-3g_ntfsprogs-2026.9.28.tgz https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-2026.9.28.tgz \
      350d9415c59f7c3fa74e23985ad2d56423c6a168337368f672e2e362d8f295c8
fetch e2fsprogs-1.47.4.tar.xz \
      https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.xz \
      fd5bf388cbdbe006a3d3b318d983b2948382440acc85a87f1e7d108653e8db0b

source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
unset DESTDIR

rm -rf ntfs-3g && mkdir ntfs-3g && tar -xzf ntfs-3g_ntfsprogs-2026.9.28.tgz -C ntfs-3g --strip-components=1
# Rohgeräte der PS5 (FreeBSD) verlangen sektorgenaue Zugriffe, siehe patches/make_unix_io_patch.py
python3 "$(dirname "$OUT")/patches/make_unix_io_patch.py" ntfs-3g/libntfs-3g/unix_io.c
(cd ntfs-3g &&
 ./configure --host=x86_64-pc-freebsd --prefix=/usr --enable-static --disable-shared \
     --disable-ntfs-3g --enable-ntfsprogs --disable-crypto --disable-plugins --disable-mtab \
     --disable-posix-acls --disable-xattr-mappings --without-uuid --without-hd --disable-ldconfig &&
 make -C libntfs-3g && make -C ntfsprogs mkntfs)

rm -rf e2fsprogs && mkdir e2fsprogs && tar -xJf e2fsprogs-1.47.4.tar.xz -C e2fsprogs --strip-components=1
(cd e2fsprogs && mkdir build && cd build &&
 ../configure --host=x86_64-pc-freebsd --prefix=/usr BUILD_CC=gcc \
     --disable-nls --disable-fuse2fs --disable-e2initrd-helper --disable-debugfs --disable-imager \
     --disable-resizer --disable-defrag --disable-uuidd --disable-backtrace --disable-mmp \
     --disable-tdb --disable-bmap-stats --without-libarchive --disable-elf-shlibs --disable-bsd-shlibs &&
 make libs && make -C misc mke2fs)

cp ntfs-3g/ntfsprogs/mkntfs "$OUT/mkntfs.elf"
cp e2fsprogs/build/misc/mke2fs "$OUT/mke2fs.elf"
"$STRIP" "$OUT/mkntfs.elf" "$OUT/mke2fs.elf"
ls -l "$OUT"
