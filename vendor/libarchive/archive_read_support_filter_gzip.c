/*-
 * Copyright (c) 2003-2007 Tim Kientzle
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

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

#include "archive_entry.h"

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#error "archive_entry.h must be included"
#endif

#include "archive_private.h"

#ifndef ARCHIVE_PRIVATE_H_INCLUDED
#error "archive_private.h must be included"
#endif

#include "archive_read_private.h"

#ifndef ARCHIVE_READ_PRIVATE_H_INCLUDED
#error "archive_read_private.h must be included"
#endif

#ifdef HAVE_ZLIB_H
struct private_data {
  z_stream stream;
  char in_stream;
  unsigned char *out_block;
  size_t out_block_size;
  int64_t total_out;
  unsigned long crc;
  uint32_t mtime;
  char *name;
  char eof;
};

static ssize_t gzip_filter_read(struct archive_read_filter *, const void **);
static int gzip_filter_close(struct archive_read_filter *);
#endif

static int gzip_bidder_bid(struct archive_read_filter_bidder *,
                           struct archive_read_filter *);
static int gzip_bidder_init(struct archive_read_filter *);

#if ARCHIVE_VERSION_NUMBER < 4000000

int archive_read_support_compression_gzip(struct archive *a) {
  return archive_read_support_filter_gzip(a);
}
#endif

static const struct archive_read_filter_bidder_vtable gzip_bidder_vtable = {
    .bid = gzip_bidder_bid,
    .init = gzip_bidder_init,
};

int archive_read_support_filter_gzip(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;

  if (__archive_read_register_bidder(a, NULL, "gzip", &gzip_bidder_vtable) !=
      ARCHIVE_OK)
    return (ARCHIVE_FATAL);

#if HAVE_ZLIB_H
  return (ARCHIVE_OK);
#else
  archive_set_error(_a, ARCHIVE_ERRNO_MISC, "Using external gzip program");
  return (ARCHIVE_WARN);
#endif
}

#define MAX_FILENAME_LENGTH (1024 * 1024L)
#define MAX_COMMENT_LENGTH (1024 * 1024L)
static ssize_t peek_at_header(struct archive_read_filter *filter, int *pbits,
#ifdef HAVE_ZLIB_H
                              struct private_data *state
#else
                              void *state
#endif
) {
  const unsigned char *p;
  ssize_t avail, len;
  int bits = 0;
  int header_flags;
#ifndef HAVE_ZLIB_H
  (void)state;
#endif

  len = 10;
  p = __archive_read_filter_ahead(filter, len, &avail);
  if (p == NULL || avail == 0)
    return (0);

  if (memcmp(p, "\x1F\x8B\x08", 3) != 0)
    return (0);
  bits += 24;
  if ((p[3] & 0xE0) != 0)
    return (0);
  bits += 3;
  header_flags = p[3];

#ifdef HAVE_ZLIB_H
  if (state)
    state->mtime = archive_le32dec(p + 4);
#endif

  if (header_flags & 4) {
    p = __archive_read_filter_ahead(filter, len + 2, &avail);
    if (p == NULL)
      return (0);
    len += ((int)p[len + 1] << 8) | (int)p[len];
    len += 2;
  }

  if (header_flags & 8) {
#ifdef HAVE_ZLIB_H
    ssize_t file_start = len;
#endif
    do {
      ++len;
      if (avail < len) {
        if (avail > MAX_FILENAME_LENGTH) {
          return (0);
        }
        p = __archive_read_filter_ahead(filter, len, &avail);
      }
      if (p == NULL)
        return (0);
    } while (p[len - 1] != 0);

#ifdef HAVE_ZLIB_H
    if (state) {

      free(state->name);
      state->name = strdup((const char *)&p[file_start]);
    }
#endif
  }

  if (header_flags & 16) {
    do {
      ++len;
      if (avail < len) {
        if (avail > MAX_COMMENT_LENGTH) {
          return (0);
        }
        p = __archive_read_filter_ahead(filter, len, &avail);
      }
      if (p == NULL)
        return (0);
    } while (p[len - 1] != 0);
  }

  if ((header_flags & 2)) {
    p = __archive_read_filter_ahead(filter, len + 2, &avail);
    if (p == NULL)
      return (0);
#if 0
	int hcrc = ((int)p[len + 1] << 8) | (int)p[len];
	int crc = ;
	if (crc != hcrc)
		return (0);
	bits += 16;
#endif
    len += 2;
  }

  if (pbits != NULL)
    *pbits = bits;
  return (len);
}

static int gzip_bidder_bid(struct archive_read_filter_bidder *self,
                           struct archive_read_filter *filter) {
  int bits_checked;

  (void)self;

  if (peek_at_header(filter, &bits_checked, NULL))
    return (bits_checked);
  return (0);
}

#ifndef HAVE_ZLIB_H

static int gzip_bidder_init(struct archive_read_filter *self) {
  int r;

  r = __archive_read_program(self, "gzip -d");

  self->code = ARCHIVE_FILTER_GZIP;
  self->name = "gzip";
  return (r);
}

#else

static int gzip_read_header(struct archive_read_filter *self,
                            struct archive_entry *entry) {
  struct private_data *state;

  state = (struct private_data *)self->data;

  if (state->mtime != 0)
    archive_entry_set_mtime(entry, state->mtime, 0);

  if (state->name)
    archive_entry_set_pathname(entry, state->name);

  return (ARCHIVE_OK);
}

static const struct archive_read_filter_vtable gzip_reader_vtable = {
    .read = gzip_filter_read,
    .close = gzip_filter_close,
#ifdef HAVE_ZLIB_H
    .read_header = gzip_read_header,
#endif
};

static int gzip_bidder_init(struct archive_read_filter *self) {
  struct private_data *state;
  static const size_t out_block_size = 64 * 1024;
  void *out_block;

  self->code = ARCHIVE_FILTER_GZIP;
  self->name = "gzip";

  state = calloc(1, sizeof(*state));
  out_block = malloc(out_block_size);
  if (state == NULL || out_block == NULL) {
    free(out_block);
    free(state);
    archive_set_error(&self->archive->archive, ENOMEM,
                      "Can't allocate data for gzip decompression");
    return (ARCHIVE_FATAL);
  }

  self->data = state;
  state->out_block_size = out_block_size;
  state->out_block = out_block;
  self->vtable = &gzip_reader_vtable;

  state->in_stream = 0;

  return (ARCHIVE_OK);
}

static int consume_header(struct archive_read_filter *self) {
  struct private_data *state;
  ssize_t avail;
  size_t len;
  int ret;

  state = (struct private_data *)self->data;

  len = peek_at_header(self->upstream, NULL, state);
  if (len == 0)
    return (ARCHIVE_EOF);
  __archive_read_filter_consume(self->upstream, len);

  state->crc = crc32(0L, NULL, 0);

  state->stream.next_in =
      (unsigned char *)(uintptr_t)__archive_read_filter_ahead(self->upstream, 1,
                                                              &avail);
  state->stream.avail_in = (uInt)avail;
  ret = inflateInit2(&(state->stream), -15);

  switch (ret) {
  case Z_OK:
    state->in_stream = 1;
    return (ARCHIVE_OK);
  case Z_STREAM_ERROR:
    archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                      "Internal error initializing compression library: "
                      "invalid setup parameter");
    break;
  case Z_MEM_ERROR:
    archive_set_error(&self->archive->archive, ENOMEM,
                      "Internal error initializing compression library: "
                      "out of memory");
    break;
  case Z_VERSION_ERROR:
    archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                      "Internal error initializing compression library: "
                      "invalid library version");
    break;
  default:
    archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                      "Internal error initializing compression library: "
                      " Zlib error %d",
                      ret);
    break;
  }
  return (ARCHIVE_FATAL);
}

static int consume_trailer(struct archive_read_filter *self) {
  struct private_data *state;
  const unsigned char *p;
  ssize_t avail;

  state = (struct private_data *)self->data;

  state->in_stream = 0;
  switch (inflateEnd(&(state->stream))) {
  case Z_OK:
    break;
  default:
    archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                      "Failed to clean up gzip decompressor");
    return (ARCHIVE_FATAL);
  }

  p = __archive_read_filter_ahead(self->upstream, 8, &avail);
  if (p == NULL || avail == 0)
    return (ARCHIVE_FATAL);

  __archive_read_filter_consume(self->upstream, 8);

  return (ARCHIVE_OK);
}

static ssize_t gzip_filter_read(struct archive_read_filter *self,
                                const void **p) {
  struct private_data *state;
  size_t decompressed;
  ssize_t avail_in, max_in;
  int ret;

  state = (struct private_data *)self->data;

  state->stream.next_out = state->out_block;
  state->stream.avail_out = (uInt)state->out_block_size;

  while (state->stream.avail_out > 0 && !state->eof) {

    if (!state->in_stream) {
      ret = consume_header(self);
      if (ret == ARCHIVE_EOF) {
        state->eof = 1;
        break;
      }
      if (ret < ARCHIVE_OK)
        return (ret);
    }

    state->stream.next_in =
        (unsigned char *)(uintptr_t)__archive_read_filter_ahead(self->upstream,
                                                                1, &avail_in);
    if (state->stream.next_in == NULL) {
      archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                        "truncated gzip input");
      return (ARCHIVE_FATAL);
    }
    if (UINT_MAX >= SSIZE_MAX)
      max_in = SSIZE_MAX;
    else
      max_in = UINT_MAX;
    if (avail_in > max_in)
      avail_in = max_in;
    state->stream.avail_in = (uInt)avail_in;

    ret = inflate(&(state->stream), 0);
    switch (ret) {
    case Z_OK:
      __archive_read_filter_consume(self->upstream,
                                    avail_in - state->stream.avail_in);
      break;
    case Z_STREAM_END:
      __archive_read_filter_consume(self->upstream,
                                    avail_in - state->stream.avail_in);

      ret = consume_trailer(self);
      if (ret < ARCHIVE_OK)
        return (ret);
      break;
    default:

      archive_set_error(&self->archive->archive, ARCHIVE_ERRNO_MISC,
                        "gzip decompression failed");
      return (ARCHIVE_FATAL);
    }
  }

  decompressed = state->stream.next_out - state->out_block;
  state->total_out += decompressed;
  if (decompressed == 0)
    *p = NULL;
  else
    *p = state->out_block;
  return (decompressed);
}

static int gzip_filter_close(struct archive_read_filter *self) {
  struct private_data *state;
  int ret;

  state = (struct private_data *)self->data;
  ret = ARCHIVE_OK;

  if (state->in_stream) {
    switch (inflateEnd(&(state->stream))) {
    case Z_OK:
      break;
    default:
      archive_set_error(&(self->archive->archive), ARCHIVE_ERRNO_MISC,
                        "Failed to clean up gzip compressor");
      ret = ARCHIVE_FATAL;
    }
  }

  free(state->name);
  free(state->out_block);
  free(state);
  return (ret);
}

#endif
