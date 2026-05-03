/*-
 * Copyright (c) 2009 Joerg  Sonnenberger
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ARCHIVE_CRC32_H
#define ARCHIVE_CRC32_H

#include "archive_platform.h"

#ifndef __LIBARCHIVE_BUILD
#error This header is only to be used internally to libarchive.
#endif

#include <stddef.h>

static unsigned long crc32(unsigned long crc, const void *_p, size_t len) {
  unsigned long crc2, b, i;
  const unsigned char *p = (const unsigned char *)_p;
  static volatile int crc_tbl_inited = 0;
  static unsigned long crc_tbl[256];

  if (_p == NULL)
    return (0);

  if (!crc_tbl_inited) {
    for (b = 0; b < 256; ++b) {
      crc2 = b;
      for (i = 8; i > 0; --i) {
        if (crc2 & 1)
          crc2 = (crc2 >> 1) ^ 0xedb88320UL;
        else
          crc2 = (crc2 >> 1);
      }
      crc_tbl[b] = crc2;
    }
    crc_tbl_inited = 1;
  }

  crc = crc ^ 0xffffffffUL;

  for (; len >= 8; len -= 8) {
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
  while (len--)
    crc = crc_tbl[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return (crc ^ 0xffffffffUL);
}

#endif
