/*
 * zstd 1.5.7 (third_party/zstd, BSD-Lizenz bzw. GPL-2.0 - siehe dort LICENSE/COPYING) als eine Übersetzungseinheit:
 * Kompression und Dekompression, ohne Altformate, Mehrkern und Assembler. Eingebunden über -isystem
 * third_party/zstd/lib, damit die strengen Warnungen des Projekts nicht für den fremden Code gelten.
 */
#define ZSTD_DISABLE_ASM 1
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_TRACE 0
#define ZSTD_NO_INTRINSICS_IF_UNAVAILABLE 1

#include <common/debug.c>
#include <common/entropy_common.c>
#include <common/error_private.c>
#include <common/fse_decompress.c>
#include <common/pool.c>
#include <common/threading.c>
#include <common/xxhash.c>
#include <common/zstd_common.c>

#include <compress/fse_compress.c>
#include <compress/hist.c>
#include <compress/huf_compress.c>
#include <compress/zstd_compress.c>
#include <compress/zstd_compress_literals.c>
#include <compress/zstd_compress_sequences.c>
#include <compress/zstd_compress_superblock.c>
#include <compress/zstd_double_fast.c>
#include <compress/zstd_fast.c>
#include <compress/zstd_lazy.c>
#include <compress/zstd_ldm.c>
#include <compress/zstd_opt.c>
#include <compress/zstd_preSplit.c>
#include <compress/zstdmt_compress.c>

#include <decompress/huf_decompress.c>
#include <decompress/zstd_ddict.c>
#include <decompress/zstd_decompress.c>
#include <decompress/zstd_decompress_block.c>
