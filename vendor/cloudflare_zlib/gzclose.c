/* gzclose.c -- zlib gzclose() function
 * Copyright (C) 2004, 2010 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzguts.h"

int ZEXPORT gzclose(gzFile file) {
#ifndef NO_GZCOMPRESS
  gz_statep state;

  if (file == NULL)
    return Z_STREAM_ERROR;
  state = (gz_statep)file;

  return state->mode == GZ_READ ? gzclose_r(file) : gzclose_w(file);
#else
  return gzclose_r(file);
#endif
}
