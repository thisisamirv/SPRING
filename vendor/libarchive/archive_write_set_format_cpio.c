#include "archive.h"
#include "archive_platform.h"

/*
 * Set output format to the default 'cpio' format.
 */
int archive_write_set_format_cpio(struct archive *_a) {
  return archive_write_set_format_cpio_odc(_a);
}
