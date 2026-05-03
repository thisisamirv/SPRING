#ifndef LIB_DEFLATE_COMPRESS_H
#define LIB_DEFLATE_COMPRESS_H

#include "libdeflate.h"

struct libdeflate_compressor;

unsigned int libdeflate_get_compression_level(struct libdeflate_compressor *c);

#endif
