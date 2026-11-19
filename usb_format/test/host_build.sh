#!/usr/bin/env bash
# Baut den Formatierer für Linux (Test-Plattform platform_host.c), samt fsck.exfat aus third_party/exfatprogs.
# Aufruf: test/host_build.sh <ausgabe> [weitere gcc-Argumente, z. B. -DSERVER_PORT=18094]
# HOST_CFLAGS (Umgebung) kommt dazu, z. B. HOST_CFLAGS="-fsanitize=address,undefined -g" für alle Testreihen
set -eu
cd "$(dirname "$0")/.."
OUT=$1
shift
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT
# gepackte Weboberfläche und Anleitung wie im Makefile (server.c bettet sie ein); mv: parallele Läufe stören sich nicht
mkdir -p build/web
for f in index.html help.html; do gzip -9nc "web/$f" >"build/web/$f.gz.$$" && mv "build/web/$f.gz.$$" "build/web/$f.gz"; done
OBJS=$(bash test/build_exfatprogs.sh "$OBJ" | tail -1)
# Spielstände: SQLite, cJSON, libdeflate (wie im Makefile, ohne die strengen Warnungen)
# shellcheck disable=SC2086
gcc -Os -w ${HOST_CFLAGS:-} -Ithird_party/sqlite3 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL     -c third_party/sqlite3/sqlite3.c -o "$OBJ/sqlite3.o"
# shellcheck disable=SC2086
gcc -Os -w ${HOST_CFLAGS:-} -Ithird_party/cjson -c third_party/cjson/cJSON.c -o "$OBJ/cJSON.o"
for f in crc32 deflate_decompress utils x86/cpu_features; do
    # shellcheck disable=SC2086
    gcc -O2 -w ${HOST_CFLAGS:-} -Ithird_party/libdeflate -c "third_party/libdeflate/lib/$f.c" -o "$OBJ/ld_$(basename $f).o"
done
SAVE3P="$OBJ/sqlite3.o $OBJ/cJSON.o $OBJ/ld_crc32.o $OBJ/ld_deflate_decompress.o $OBJ/ld_utils.o $OBJ/ld_cpu_features.o"
# shellcheck disable=SC2086
gcc -O2 -w -DNDEBUG ${HOST_CFLAGS:-} -isystem third_party/zstd/lib -c src/zstd_all.c -o "$OBJ/zstd_all.o"
# shellcheck disable=SC2086
gcc -Wall -Wextra -Werror -O2 -DUSBFMT_HOST "-DUSBFMT_VERSION=\"$(cat ../VERSION)\"" -isystem third_party/zstd/lib \
    -isystem third_party/sqlite3 -isystem third_party/cjson -isystem third_party/libdeflate ${HOST_CFLAGS:-} "$@" -o "$OUT" \
    src/main.c src/server.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c src/extfs.c src/fsdetect.c src/fsread.c src/vollabel.c \
    src/devtest.c src/library.c src/smpapi.c src/sha256.c src/imgbak.c src/saves.c src/savesfo.c src/chives.c src/savezip.c src/savedb.c \
    src/savemnt.c src/platform_host.c $OBJS $SAVE3P "$OBJ/zstd_all.o" -lpthread -lm
