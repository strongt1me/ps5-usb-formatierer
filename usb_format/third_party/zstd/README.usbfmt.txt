zstd 1.5.7 (Meta Platforms) - for the image backup (.img.zst) in the formatter and in the PC helpers.

Source:  https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
SHA-256: eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
Licence: BSD (LICENSE) or alternatively GPL-2.0 (COPYING) - used under the BSD licence.

Only lib/common, lib/compress, lib/decompress, lib/zstd.h and lib/zstd_errors.h are included, unchanged except for
leaving out decompress/huf_decompress_amd64.S (assembler; built with ZSTD_DISABLE_ASM). Everything is compiled as a
single unit: src/zstd_all.c.
