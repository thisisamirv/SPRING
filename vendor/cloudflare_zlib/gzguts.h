/* gzguts.h -- zlib internal header definitions for gz* operations
 * Copyright (C) 2004-2019 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef _LARGEFILE64_SOURCE
#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE 1
#endif
#undef _FILE_OFFSET_BITS
#undef _TIME_BITS
#endif

#ifdef HAVE_HIDDEN
#define ZLIB_INTERNAL __attribute__((visibility("hidden")))
#else
#define ZLIB_INTERNAL
#endif

#include "zlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void _z_gz_ide_unused_fix(void) {
  (void)fprintf;
  (void)malloc;
  (void)free;
  (void)memset;
  (void)memcpy;
  (void)strlen;
}

#ifdef STDC
#include <limits.h>
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <fcntl.h>

#ifdef _WIN32
#include <stddef.h>
#endif

#if defined(__TURBOC__) || defined(_MSC_VER) || defined(_WIN32)
#include <io.h>
#endif

#if defined(_WIN32)
#define WIDECHAR
#endif

#ifdef WINAPI_FAMILY
#define open _open
#define read _read
#define write _write
#define close _close
#endif

#ifdef NO_DEFLATE
#define NO_GZCOMPRESS
#endif

#if defined(STDC99) || (defined(__TURBOC__) && __TURBOC__ >= 0x550)
#ifndef HAVE_VSNPRINTF
#define HAVE_VSNPRINTF
#endif
#endif

#if defined(__CYGWIN__)
#ifndef HAVE_VSNPRINTF
#define HAVE_VSNPRINTF
#endif
#endif

#if defined(MSDOS) && defined(__BORLANDC__) && (BORLANDC > 0x410)
#ifndef HAVE_VSNPRINTF
#define HAVE_VSNPRINTF
#endif
#endif

#ifndef HAVE_VSNPRINTF
#if !defined(NO_vsnprintf) &&                                                  \
    (defined(MSDOS) || defined(__TURBOC__) || defined(__SASC) ||               \
     defined(VMS) || defined(__OS400) || defined(__MVS__))

#define NO_vsnprintf
#endif
#ifdef WIN32

#if !defined(_MSC_VER) || (defined(_MSC_VER) && _MSC_VER < 1500)
#ifndef vsnprintf
#define vsnprintf _vsnprintf
#endif
#endif
#elif !defined(__STDC_VERSION__) || __STDC_VERSION__ - 0 < 199901L

#ifndef NO_snprintf
#define NO_snprintf
#endif
#ifndef NO_vsnprintf
#define NO_vsnprintf
#endif
#endif
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

#ifndef local
#define local static
#endif

#ifndef STDC
extern voidp malloc(uInt size);
extern void free(voidpf ptr);
#endif

#if defined UNDER_CE
#include <windows.h>
#define zstrerror() gz_strwinerror((DWORD)GetLastError())
#else
#ifndef NO_STRERROR
#include <errno.h>
#define zstrerror() strerror(errno)
#else
#define zstrerror() "stdio error (consult errno)"
#endif
#endif

#if !defined(_LARGEFILE64_SOURCE) || _LFS64_LARGEFILE - 0 == 0
ZEXTERN gzFile ZEXPORT gzopen64(const char *, const char *);
ZEXTERN z_off64_t ZEXPORT gzseek64(gzFile, z_off64_t, int);
ZEXTERN z_off64_t ZEXPORT gztell64(gzFile);
ZEXTERN z_off64_t ZEXPORT gzoffset64(gzFile);
#endif

#if MAX_MEM_LEVEL >= 8
#define DEF_MEM_LEVEL 8
#else
#define DEF_MEM_LEVEL MAX_MEM_LEVEL
#endif

#define GZBUFSIZE 8192

#define GZ_NONE 0
#define GZ_READ 7247
#define GZ_WRITE 31153
#define GZ_APPEND 1

#define LOOK 0
#define COPY 1
#define GZIP 2

typedef struct {

  struct gzFile_s x;

  int mode;
  int fd;
  char *path;
  unsigned size;
  unsigned want;
  unsigned char *in;
  unsigned char *out;
  int direct;

  int how;
  z_off64_t start;
  int eof;
  int past;

  int level;
  int strategy;
  int reset;

  z_off64_t skip;
  int seek;

  int err;
  char *msg;

  z_stream strm;
} gz_state;
typedef gz_state FAR *gz_statep;

void ZLIB_INTERNAL gz_error(gz_statep, int, const char *);
#if defined UNDER_CE
char ZLIB_INTERNAL *gz_strwinerror(DWORD error);
#endif

#ifdef INT_MAX
#define GT_OFF(x) (sizeof(int) == sizeof(z_off64_t) && (x) > INT_MAX)
#else
unsigned ZLIB_INTERNAL gz_intmax(void);
#define GT_OFF(x) (sizeof(int) == sizeof(z_off64_t) && (x) > gz_intmax())
#endif
