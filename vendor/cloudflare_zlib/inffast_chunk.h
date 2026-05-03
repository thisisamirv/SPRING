/* inffast_chunk.h -- header to use inffast_chunk.c
 *
 * (C) 1995-2013 Jean-loup Gailly and Mark Adler
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Jean-loup Gailly        Mark Adler
 * jloup@gzip.org          madler@alumni.caltech.edu
 *
 * Copyright (C) 1995-2003, 2010 Mark Adler
 * Copyright (C) 2017 ARM, Inc.
 * Copyright 2023 The Chromium Authors
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zutil.h"

#ifdef INFLATE_CHUNK_READ_64LE
#undef INFLATE_FAST_MIN_INPUT
#define INFLATE_FAST_MIN_INPUT 15
#undef INFLATE_FAST_MIN_OUTPUT
#define INFLATE_FAST_MIN_OUTPUT 260
#endif

void ZLIB_INTERNAL inflate_fast_chunk_(z_streamp strm, unsigned start);
