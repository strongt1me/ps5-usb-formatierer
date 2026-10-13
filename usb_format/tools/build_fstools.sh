#!/usr/bin/env bash
# Baut mkntfs (ntfs-3g 2022.10.3) und mke2fs (e2fsprogs 1.47.1) für die PS5 nach tools/bin/.
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
fetch ntfs-3g_ntfsprogs-2022.10.3.tgz https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-2022.10.3.tgz \
      f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c
fetch e2fsprogs-1.47.1.tar.xz \
      https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.1/e2fsprogs-1.47.1.tar.xz \
      5a33dc047fd47284bca4bb10c13cfe7896377ae3d01cb81a05d406025d99e0d1

source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
unset DESTDIR

rm -rf ntfs-3g && mkdir ntfs-3g && tar -xzf ntfs-3g_ntfsprogs-2022.10.3.tgz -C ntfs-3g --strip-components=1
# Rohgeräte der PS5 (FreeBSD) verlangen sektorgenaue Zugriffe, siehe patches/make_unix_io_patch.py
python3 "$(dirname "$OUT")/patches/make_unix_io_patch.py" ntfs-3g/libntfs-3g/unix_io.c
(cd ntfs-3g &&
 ./configure --host=x86_64-pc-freebsd --prefix=/usr --enable-static --disable-shared \
     --disable-ntfs-3g --enable-ntfsprogs --disable-crypto --disable-plugins --disable-mtab \
     --disable-posix-acls --disable-xattr-mappings --without-uuid --without-hd --disable-ldconfig &&
 make -C libntfs-3g && make -C ntfsprogs mkntfs)

rm -rf e2fsprogs && mkdir e2fsprogs && tar -xJf e2fsprogs-1.47.1.tar.xz -C e2fsprogs --strip-components=1
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
