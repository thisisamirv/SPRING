#ifndef INFFAST_H
#define INFFAST_H

#include "zutil.h"

#define INFLATE_FAST_MIN_INPUT 6

#define INFLATE_FAST_MIN_OUTPUT 258

void ZLIB_INTERNAL inflate_fast(z_streamp strm, unsigned start);

#endif
