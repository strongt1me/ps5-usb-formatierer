#!/usr/bin/env bash
# SHA-256: Prüfwerte, Hardware-Fassung (SHA-NI) gegen die einfache; auch mit den Sanitizern (HOST_CFLAGS)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# shellcheck disable=SC2086
gcc -Wall -Wextra -Werror -O2 ${HOST_CFLAGS:-} -o "$W/sha" test/sha_test.c src/sha256.c || exit 1
"$W/sha"
