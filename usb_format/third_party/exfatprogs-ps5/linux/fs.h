/* Ersatz für <linux/fs.h> beim Übersetzen von exfatprogs für die PS5: Die Linux-ioctls gibt es dort nicht.
 * Sie werden nur in Funktionen benutzt, die der Formatierer nicht aufruft (Geräteinfo kommt aus exfat_fsck.c).
 * Unter Linux bringt <linux/fs.h> über <linux/types.h> auch die Kernel-Ganzzahltypen mit. */
#ifndef USBFMT_LINUX_FS_H
#define USBFMT_LINUX_FS_H
#include <stdint.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
#define BLKSSZGET   0x7fff0001UL
#define BLKDISCARD  0x7fff0002UL
#endif
