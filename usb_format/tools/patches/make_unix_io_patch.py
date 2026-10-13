"""Ändert libntfs-3g/unix_io.c (ntfs-3g 2022.10.3): Ein-/Ausgabe auf Zeichengeräten sektorgenau ausrichten.
FreeBSD-Rohgeräte (so auch /dev/daNsM auf der PS5) lehnen Zugriffe ab, die nicht auf Sektorgrenzen beginnen
und enden (EINVAL) - mkntfs scheiterte daran mit "Couldn't create root directory: Invalid argument".
Aufruf: python3 make_unix_io_patch.py <unix_io.c>   (ändert die Datei an Ort und Stelle)"""
import sys

path = sys.argv[1]
src = open(path).read()


def sub(old, new, count=1):
    global src
    if src.count(old) != count:
        sys.exit(f"Muster {count}x erwartet, {src.count(old)}x gefunden:\n{old}")
    src = src.replace(old, new)


sub('#define DEV_FD(dev)	(*(int *)dev->d_private)\n', '''#define DEV_FD(dev)	(*(int *)dev->d_private)

/* d_private: Dateideskriptor vorne (DEV_FD liest den ersten int), dahinter die Sektorgröße, auf die
 * Zugriffe auszurichten sind (0 = nicht nötig) */
struct unix_filedesc {
	int fd;
	int align;
};
#define DEV_ALIGN(dev)	(((struct unix_filedesc *)dev->d_private)->align)

#if defined(__FreeBSD__)
#include <sys/disk.h>
#endif

/* Lesen über einen sektorgenauen Puffer, falls Anfang oder Länge nicht auf Sektorgrenzen liegen */
static s64 aligned_pread(int fd, int align, void *buf, s64 count, s64 offset)
{
	s64 start, end, got;
	char *bounce;

	if (!align || (!(offset % align) && !(count % align)))
		return pread(fd, buf, count, offset);
	start = offset - offset % align;
	end = offset + count;
	if (end % align)
		end += align - end % align;
	bounce = ntfs_malloc(end - start);
	if (!bounce)
		return -1;
	got = pread(fd, bounce, end - start, start);
	if (got > offset - start) {
		got -= offset - start;
		if (got > count)
			got = count;
		memcpy(buf, bounce + (offset - start), got);
	} else if (got >= 0)
		got = 0;
	free(bounce);
	return got;
}

/* Schreiben über einen sektorgenauen Puffer: angeschnittene Rand-Sektoren erst lesen, dann ganz schreiben */
static s64 aligned_pwrite(int fd, int align, const void *buf, s64 count, s64 offset)
{
	s64 start, end, len;
	char *bounce;

	if (!align || (!(offset % align) && !(count % align)))
		return pwrite(fd, buf, count, offset);
	start = offset - offset % align;
	end = offset + count;
	if (end % align)
		end += align - end % align;
	len = end - start;
	bounce = ntfs_malloc(len);
	if (!bounce)
		return -1;
	if (offset != start && pread(fd, bounce, align, start) != align)
		goto fail;
	if ((offset + count) % align && !(len == align && offset != start) &&
	    pread(fd, bounce + len - align, align, end - align) != align)
		goto fail;
	memcpy(bounce + (offset - start), buf, count);
	if (pwrite(fd, bounce, len, start) != len)
		goto fail;
	free(bounce);
	return count;
fail:
	free(bounce);
	if (!errno)
		errno = EIO;
	return -1;
}
''')

sub('	dev->d_private = ntfs_malloc(sizeof(int));\n',
    '	dev->d_private = ntfs_malloc(sizeof(struct unix_filedesc));\n')

sub('''	if ((flags & O_RDWR) != O_RDWR)
		NDevSetReadOnly(dev);
''', '''	DEV_ALIGN(dev) = 0;
#if defined(__FreeBSD__) || defined(FORCE_ALIGNED_IO)
	if (S_ISCHR(sbuf.st_mode)
#ifdef FORCE_ALIGNED_IO
	    || 1	/* Test: ausgerichteten Weg auch für normale Dateien erzwingen */
#endif
	    ) {
		u_int ss = 512;
#ifdef DIOCGSECTORSIZE
		if (ioctl(DEV_FD(dev), DIOCGSECTORSIZE, &ss) || !ss)
			ss = 512;
#endif
		DEV_ALIGN(dev) = (int)ss;
	}
#endif

	if ((flags & O_RDWR) != O_RDWR)
		NDevSetReadOnly(dev);
''')

sub('''	return read(DEV_FD(dev), buf, count);
''', '''	s64 pos, n;

	if (!DEV_ALIGN(dev))
		return read(DEV_FD(dev), buf, count);
	pos = lseek(DEV_FD(dev), 0, SEEK_CUR);
	if (pos < 0)
		return -1;
	n = aligned_pread(DEV_FD(dev), DEV_ALIGN(dev), buf, count, pos);
	if (n > 0)
		lseek(DEV_FD(dev), pos + n, SEEK_SET);
	return n;
''')

sub('''	NDevSetDirty(dev);
	return write(DEV_FD(dev), buf, count);
''', '''	NDevSetDirty(dev);
	if (DEV_ALIGN(dev)) {
		s64 pos = lseek(DEV_FD(dev), 0, SEEK_CUR), n;
		if (pos < 0)
			return -1;
		n = aligned_pwrite(DEV_FD(dev), DEV_ALIGN(dev), buf, count, pos);
		if (n > 0)
			lseek(DEV_FD(dev), pos + n, SEEK_SET);
		return n;
	}
	return write(DEV_FD(dev), buf, count);
''')

sub('	return pread(DEV_FD(dev), buf, count, offset);\n',
    '	return aligned_pread(DEV_FD(dev), DEV_ALIGN(dev), buf, count, offset);\n')
sub('	return pwrite(DEV_FD(dev), buf, count, offset);\n',
    '	return aligned_pwrite(DEV_FD(dev), DEV_ALIGN(dev), buf, count, offset);\n')

open(path, "w").write(src)
print("unix_io.c angepasst:", path)
