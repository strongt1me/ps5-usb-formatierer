#!/usr/bin/env bash
# Spielstand-API (Liste, ZIP-Sicherung, Wiederherstellen, Schutzregeln) gegen den Host-Formatierer.
# Aufruf: test/saves_test.sh   (baut selbst, Port 18115)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18115 || exit 1
python3 test/saves_test.py "$W/srv" 18115
