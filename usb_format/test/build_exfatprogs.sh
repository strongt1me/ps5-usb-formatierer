#!/usr/bin/env bash
# Übersetzt die unveränderten exfatprogs-Quellen (fsck.exfat) mit exfat_shim.h für den Host-Formatierer.
# Aufruf: test/build_exfatprogs.sh <zielordner>   -> legt dort *.o ab und gibt die Liste aus
set -eu
cd "$(dirname "$0")/.."
OUT=$1
mkdir -p "$OUT"
E=third_party/exfatprogs
FLAGS=(-O2 -w -DNDEBUG -D_GNU_SOURCE -include src/exfat_shim.h -I $E/include -I $E/fsck)
gcc "${FLAGS[@]}" -Dexfat_get_blk_dev_info=exfatprogs_get_blk_dev_info_orig -c $E/lib/libexfat.c -o "$OUT/ep_libexfat.o"
gcc "${FLAGS[@]}" -c $E/lib/exfat_fs.c -o "$OUT/ep_exfat_fs.o"
gcc "${FLAGS[@]}" -c $E/lib/exfat_dir.c -o "$OUT/ep_exfat_dir.o"
gcc "${FLAGS[@]}" -c $E/lib/utils.c -o "$OUT/ep_utils.o"
gcc "${FLAGS[@]}" -Dmain=exfatprogs_fsck_main -c $E/fsck/fsck.c -o "$OUT/ep_fsck.o"
gcc "${FLAGS[@]}" -c $E/fsck/repair.c -o "$OUT/ep_repair.o"
gcc -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I $E/include -I $E/fsck -c src/exfat_fsck.c -o "$OUT/exfat_fsck.o"
echo "$OUT/ep_libexfat.o $OUT/ep_exfat_fs.o $OUT/ep_exfat_dir.o $OUT/ep_utils.o $OUT/ep_fsck.o $OUT/ep_repair.o $OUT/exfat_fsck.o"
