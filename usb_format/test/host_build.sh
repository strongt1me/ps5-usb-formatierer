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
OBJS=$(bash test/build_exfatprogs.sh "$OBJ" | tail -1)
# shellcheck disable=SC2086
gcc -Wall -Wextra -Werror -O2 -DUSBFMT_HOST ${HOST_CFLAGS:-} "$@" -o "$OUT" \
    src/main.c src/server.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c src/extfs.c src/fsdetect.c src/fsread.c src/vollabel.c \
    src/devtest.c src/library.c src/smpapi.c src/platform_host.c $OBJS -lpthread
