/* Windows-Bau von mke2fs: "mke2fs -d archiv.tar" (libarchive) wird nicht gebraucht */
#include "config.h"
#include "ext2fs/ext2fs.h"

struct hdlinks_s;
struct file_info;
struct fs_ops_callbacks;

errcode_t __populate_fs_from_tar(ext2_filsys fs, ext2_ino_t root_ino, const char *source_tar, ext2_ino_t root,
                                 struct hdlinks_s *hdlinks, struct file_info *target,
                                 struct fs_ops_callbacks *fs_callbacks) {
    (void)fs; (void)root_ino; (void)source_tar; (void)root; (void)hdlinks; (void)target; (void)fs_callbacks;
    return EXT2_ET_UNIMPLEMENTED;
}
