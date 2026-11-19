#!/usr/bin/env bash
# Baut den Formatierer für Linux (Test-Plattform platform_host.c), samt fsck.exfat aus third_party/exfatprogs.
# Aufruf: test/host_build.sh <ausgabe> [weitere gcc-Argumente, z. B. -DSERVER_PORT=18094]
# HOST_CFLAGS (Umgebung) kommt dazu, z. B. HOST_CFLAGS="-fsanitize=address,undefined -g" für alle Testreihen
set -eu
cd "$(dirname "$0")/.."
OUT=$1
shift
OBJ=$(mktemp -d)
NEW=
trap 'rm -rf "$OBJ" ${NEW:+"$NEW"}' EXIT   # NEW: ein abgebrochener Bau der Fremdbibliotheken
# gepackte Weboberfläche und Anleitung wie im Makefile (server.c bettet sie ein); mv: parallele Läufe stören sich nicht
mkdir -p build/web
for f in index.html help.html; do gzip -9nc "web/$f" >"build/web/$f.gz.$$" && mv "build/web/$f.gz.$$" "build/web/$f.gz"; done
OBJS=$(bash test/build_exfatprogs.sh "$OBJ" | tail -1)
# Fremdbibliotheken (SQLite, cJSON, libdeflate, zstd) nur einmal je Stand und Einstellung übersetzen: SQLite allein braucht
# mit Sanitizern Minuten, und jede Testreihe baut neu. Der Schlüssel umfasst Compiler, HOST_CFLAGS, Quellen und dieses Skript.
T3P=build/host3p-$({ gcc --version | head -1; printf '%s\n' "${HOST_CFLAGS:-}"; cat test/host_build.sh src/zstd_all.c
                     find third_party/sqlite3 third_party/cjson third_party/libdeflate third_party/zstd/lib -type f | LC_ALL=C sort | xargs cat
                   } | sha1sum | cut -c1-16)
if [ ! -f "$T3P/fertig" ]; then
    NEW=$(mktemp -d build/host3p-neu.XXXXXX)
    # Spielstände: SQLite, cJSON, libdeflate (wie im Makefile, ohne die strengen Warnungen)
    # shellcheck disable=SC2086
    gcc -Os -w ${HOST_CFLAGS:-} -Ithird_party/sqlite3 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL     -c third_party/sqlite3/sqlite3.c -o "$NEW/sqlite3.o"
    # shellcheck disable=SC2086
    gcc -Os -w ${HOST_CFLAGS:-} -Ithird_party/cjson -c third_party/cjson/cJSON.c -o "$NEW/cJSON.o"
    for f in crc32 deflate_decompress utils x86/cpu_features; do
        # shellcheck disable=SC2086
        gcc -O2 -w ${HOST_CFLAGS:-} -Ithird_party/libdeflate -c "third_party/libdeflate/lib/$f.c" -o "$NEW/ld_$(basename $f).o"
    done
    # shellcheck disable=SC2086
    gcc -O2 -w -DNDEBUG ${HOST_CFLAGS:-} -isystem third_party/zstd/lib -c src/zstd_all.c -o "$NEW/zstd_all.o"
    touch "$NEW/fertig"
    # erst vollständig, dann an seinen Platz; baute ein paralleler Lauf schneller, gilt dessen Ordner
    mv -T "$NEW" "$T3P" 2>/dev/null || rm -rf "$NEW"
fi
SAVE3P="$T3P/sqlite3.o $T3P/cJSON.o $T3P/ld_crc32.o $T3P/ld_deflate_decompress.o $T3P/ld_utils.o $T3P/ld_cpu_features.o"
# shellcheck disable=SC2086
gcc -Wall -Wextra -Werror -O2 -DUSBFMT_HOST "-DUSBFMT_VERSION=\"$(cat ../VERSION)\"" -isystem third_party/zstd/lib \
    -isystem third_party/sqlite3 -isystem third_party/cjson -isystem third_party/libdeflate ${HOST_CFLAGS:-} "$@" -o "$OUT" \
    src/main.c src/server.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c src/extfs.c src/fsdetect.c src/fsread.c src/vollabel.c \
    src/devtest.c src/library.c src/smpapi.c src/sha256.c src/imgbak.c src/saves.c src/savesfo.c src/chives.c src/savezip.c src/savedb.c \
    src/savemnt.c src/platform_host.c $OBJS $SAVE3P "$T3P/zstd_all.o" -lpthread -lm
