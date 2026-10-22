#!/usr/bin/env bash
# Fuzzt den ZIP-Leser der Spielstand-Wiederherstellung unter ASAN/UBSAN (siehe saves_fuzz.c).
# Aufruf: test/saves_fuzz.sh [Durchläufe, Standard 20000] [Seed]
set -eu
cd "$(dirname "$0")/.."
N=${1:-20000}
SEED=${2:-1}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Wno-format-truncation \
    -o "$W/fuzz" test/saves_fuzz.c src/saves.c src/i18n.c src/i18n_table.c -lpthread
python3 test/saves_fixture.py "$W/fx" >/dev/null
python3 - "$W" <<'PY'
import io, json, sys, zipfile, os
sys.path.insert(0, "test")
import saves_fixture as fx
w = sys.argv[1]
b = io.BytesIO()
with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as z:
    z.writestr("metadata.json", json.dumps({"title_id": "PPSA01234", "save_dir": "sdimg_SAVEDATA00", "uid": "1c3f8a0e"}))
    z.writestr("PPSA01234_sdimg_SAVEDATA00.img", fx.container("fuzz", 1 << 20))
open(os.path.join(w, "good.zip"), "wb").write(b.getvalue())
PY
mkdir -p "$W/data"
"$W/fuzz" "$W/good.zip" "$W/fx/home" "$W/data" "$N" "$SEED"
