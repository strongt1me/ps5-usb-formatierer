zstd 1.5.7 (Meta Platforms) - für die Image-Sicherung (.img.zst) im Formatierer und in den PC-Helfern.

Quelle:  https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
SHA-256: eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
Lizenz:  BSD (LICENSE) oder wahlweise GPL-2.0 (COPYING) - genutzt unter der BSD-Lizenz.

Übernommen sind nur lib/common, lib/compress, lib/decompress, lib/zstd.h und lib/zstd_errors.h, unverändert bis auf
das Weglassen von decompress/huf_decompress_amd64.S (Assembler; gebaut wird mit ZSTD_DISABLE_ASM). Übersetzt wird
alles als eine Einheit: src/zstd_all.c.
