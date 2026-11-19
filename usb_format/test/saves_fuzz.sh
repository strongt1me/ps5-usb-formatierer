#!/usr/bin/env bash
# Fuzzt das Einspielen von Spielständen unter ASAN/UBSAN (siehe saves_fuzz.c): ZIP-Leser, chives.json, param.sfo, SQLite-Zeile.
# Aufruf: test/saves_fuzz.sh [Durchläufe, Standard 3000] [Seed]
set -eu
cd "$(dirname "$0")/.."
N=${1:-3000}
SEED=${2:-1}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -Os -w -Ithird_party/sqlite3 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL -c third_party/sqlite3/sqlite3.c -o "$W/sqlite3.o"
gcc -Os -w -Ithird_party/cjson -c third_party/cjson/cJSON.c -o "$W/cJSON.o"
for f in crc32 deflate_decompress utils x86/cpu_features; do
    gcc -O2 -w -Ithird_party/libdeflate -c "third_party/libdeflate/lib/$f.c" -o "$W/ld_$(basename "$f").o"
done
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Wno-format-truncation -DUSBFMT_HOST \
    -isystem third_party/sqlite3 -isystem third_party/cjson -isystem third_party/libdeflate \
    -o "$W/fuzz" test/saves_fuzz.c src/saves.c src/savesfo.c src/chives.c src/savezip.c src/savedb.c src/savemnt.c src/i18n.c src/i18n_table.c \
    "$W/sqlite3.o" "$W/cJSON.o" "$W"/ld_*.o -lpthread -lm
python3 test/saves_fixture.py "$W/fx" >/dev/null
python3 - "$W" <<'PY'
import io, json, sys, zipfile, os
sys.path.insert(0, "test")
import saves_fixture as fx
w = sys.argv[1]
uid, tid, slot = "1c3f8a0e", "PPSA01234", "SAVEDATA00"
chives = {"schema_version": 1, "platform": "PS5", "title_id": tid, "save_directory": slot, "encrypted_name": "sdimg_" + slot,
          "original_image_size_bytes": 64 * 32768,
          "pfs": {"version": 2, "block_size_bytes": 32768, "block_count": 64, "mode_raw": 13, "backup_header_count": 1, "fs_ext": 0},
          "ownership": [{"path": "data/main.sav", "uid": 0, "gid": 0, "mode": 420}]}
b = io.BytesIO()
with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as z:
    for rel, data in fx.content(uid, tid, slot).items():
        z.writestr(rel, data)
    z.writestr("sce_sys/param.sfo", fx.save_sfo(uid, tid, slot, 64))
    z.writestr("chives.json", json.dumps(chives))
open(os.path.join(w, "good.zip"), "wb").write(b.getvalue())
PY
mkdir -p "$W/data"
USBFMT_SAVEMNT_STORE="$W/fx/store" "$W/fuzz" "$W/good.zip" "$W/fx/home" "$W/data" "$W/fx/sys" "$N" "$SEED"
