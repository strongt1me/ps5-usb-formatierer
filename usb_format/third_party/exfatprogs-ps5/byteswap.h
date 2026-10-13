/* Ersatz für glibcs <byteswap.h> beim Übersetzen von exfatprogs für die PS5 (FreeBSD-libc). */
#ifndef USBFMT_BYTESWAP_H
#define USBFMT_BYTESWAP_H
#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
#endif
