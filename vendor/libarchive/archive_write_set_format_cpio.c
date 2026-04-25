#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

/*
 * Set output format to the default 'cpio' format.
 */
int archive_write_set_format_cpio(struct archive *_a) {
  return archive_write_set_format_cpio_odc(_a);
}
