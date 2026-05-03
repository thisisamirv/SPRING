/*-
 * Copyright (c) 2003-2011 Tim Kientzle
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

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
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

#define minimum(a, b) (a < b ? a : b)

static int choose_filters(struct archive_read *);
static int choose_format(struct archive_read *);
static int close_filters(struct archive_read *);
static int64_t _archive_filter_bytes(struct archive *, int);
static int _archive_filter_code(struct archive *, int);
static const char *_archive_filter_name(struct archive *, int);
static int _archive_filter_count(struct archive *);
static int _archive_read_close(struct archive *);
static int _archive_read_data_block(struct archive *, const void **, size_t *,
                                    int64_t *);
static int _archive_read_free(struct archive *);
static int _archive_read_next_header(struct archive *, struct archive_entry **);
static int _archive_read_next_header2(struct archive *, struct archive_entry *);
static int64_t advance_file_pointer(struct archive_read_filter *, int64_t);

static const struct archive_vtable archive_read_vtable = {
    .archive_filter_bytes = _archive_filter_bytes,
    .archive_filter_code = _archive_filter_code,
    .archive_filter_name = _archive_filter_name,
    .archive_filter_count = _archive_filter_count,
    .archive_read_data_block = _archive_read_data_block,
    .archive_read_next_header = _archive_read_next_header,
    .archive_read_next_header2 = _archive_read_next_header2,
    .archive_free = _archive_read_free,
    .archive_close = _archive_read_close,
};

struct archive *archive_read_new(void) {
  struct archive_read *a;

  a = calloc(1, sizeof(*a));
  if (a == NULL)
    return (NULL);
  a->archive.magic = ARCHIVE_READ_MAGIC;

  a->archive.state = ARCHIVE_STATE_NEW;
  a->entry = archive_entry_new2(&a->archive);
  a->archive.vtable = &archive_read_vtable;

  a->passphrases.last = &a->passphrases.first;

  return (&a->archive);
}

void archive_read_extract_set_skip_file(struct archive *_a, la_int64_t d,
                                        la_int64_t i) {
  struct archive_read *a = (struct archive_read *)_a;

  if (ARCHIVE_OK != __archive_check_magic(_a, ARCHIVE_READ_MAGIC,
                                          ARCHIVE_STATE_ANY,
                                          "archive_read_extract_set_skip_file"))
    return;
  a->skip_file_set = 1;
  a->skip_file_dev = d;
  a->skip_file_ino = i;
}

int archive_read_open(struct archive *a, void *client_data,
                      archive_open_callback *client_opener,
                      archive_read_callback *client_reader,
                      archive_close_callback *client_closer) {

  archive_read_set_open_callback(a, client_opener);
  archive_read_set_read_callback(a, client_reader);
  archive_read_set_close_callback(a, client_closer);
  archive_read_set_callback_data(a, client_data);
  return archive_read_open1(a);
}

int archive_read_open2(struct archive *a, void *client_data,
                       archive_open_callback *client_opener,
                       archive_read_callback *client_reader,
                       archive_skip_callback *client_skipper,
                       archive_close_callback *client_closer) {

  archive_read_set_callback_data(a, client_data);
  archive_read_set_open_callback(a, client_opener);
  archive_read_set_read_callback(a, client_reader);
  archive_read_set_skip_callback(a, client_skipper);
  archive_read_set_close_callback(a, client_closer);
  return archive_read_open1(a);
}

static ssize_t client_read_proxy(struct archive_read_filter *self,
                                 const void **buff) {
  ssize_t r;
  r = (self->archive->client.reader)(&self->archive->archive, self->data, buff);
  return (r);
}

static int64_t client_skip_proxy(struct archive_read_filter *self,
                                 int64_t request) {
  if (request < 0)
    __archive_errx(1, "Negative skip requested.");
  if (request == 0)
    return 0;

  if (self->archive->client.skipper != NULL) {
    int64_t total = 0;
    for (;;) {
      int64_t get, ask = request;
      get = (self->archive->client.skipper)(&self->archive->archive, self->data,
                                            ask);
      total += get;
      if (get == 0 || get == request)
        return (total);
      if (get > request)
        return ARCHIVE_FATAL;
      request -= get;
    }
  } else if (self->archive->client.seeker != NULL && request > 64 * 1024) {

    int64_t before = self->position;
    int64_t after = (self->archive->client.seeker)(
        &self->archive->archive, self->data, request, SEEK_CUR);
    if (after != before + request)
      return ARCHIVE_FATAL;
    return after - before;
  }
  return 0;
}

static int64_t client_seek_proxy(struct archive_read_filter *self,
                                 int64_t offset, int whence) {

  if (self->archive->client.seeker == NULL) {
    archive_set_error(
        &self->archive->archive, ARCHIVE_ERRNO_MISC,
        "Current client reader does not support seeking a device");
    return (ARCHIVE_FAILED);
  }
  return (self->archive->client.seeker)(&self->archive->archive, self->data,
                                        offset, whence);
}

static int read_client_close_proxy(struct archive_read *a) {
  int r = ARCHIVE_OK, r2;
  unsigned int i;

  if (a->client.closer == NULL)
    return (r);
  for (i = 0; i < a->client.nodes; i++) {
    r2 = (a->client.closer)((struct archive *)a, a->client.dataset[i].data);
    if (r > r2)
      r = r2;
  }
  return (r);
}

static int client_close_proxy(struct archive_read_filter *self) {
  return read_client_close_proxy(self->archive);
}

static int client_open_proxy(struct archive_read_filter *self) {
  int r = ARCHIVE_OK;
  if (self->archive->client.opener != NULL)
    r = (self->archive->client.opener)((struct archive *)self->archive,
                                       self->data);
  return (r);
}

static int client_switch_proxy(struct archive_read_filter *self,
                               unsigned int iindex) {
  int r1 = ARCHIVE_OK, r2 = ARCHIVE_OK;
  void *data2 = NULL;

  if (self->archive->client.cursor == iindex)
    return (ARCHIVE_OK);

  self->archive->client.cursor = iindex;
  data2 = self->archive->client.dataset[self->archive->client.cursor].data;
  if (self->archive->client.switcher != NULL) {
    r1 = r2 = (self->archive->client.switcher)((struct archive *)self->archive,
                                               self->data, data2);
    self->data = data2;
  } else {

    if (self->archive->client.closer != NULL)
      r1 = (self->archive->client.closer)((struct archive *)self->archive,
                                          self->data);
    self->data = data2;
    r2 = client_open_proxy(self);
  }
  return (r1 < r2) ? r1 : r2;
}

int archive_read_set_open_callback(struct archive *_a,
                                   archive_open_callback *client_opener) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_open_callback");
  a->client.opener = client_opener;
  return ARCHIVE_OK;
}

int archive_read_set_read_callback(struct archive *_a,
                                   archive_read_callback *client_reader) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_read_callback");
  a->client.reader = client_reader;
  return ARCHIVE_OK;
}

int archive_read_set_skip_callback(struct archive *_a,
                                   archive_skip_callback *client_skipper) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_skip_callback");
  a->client.skipper = client_skipper;
  return ARCHIVE_OK;
}

int archive_read_set_seek_callback(struct archive *_a,
                                   archive_seek_callback *client_seeker) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_seek_callback");
  a->client.seeker = client_seeker;
  return ARCHIVE_OK;
}

int archive_read_set_close_callback(struct archive *_a,
                                    archive_close_callback *client_closer) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_close_callback");
  a->client.closer = client_closer;
  return ARCHIVE_OK;
}

int archive_read_set_switch_callback(struct archive *_a,
                                     archive_switch_callback *client_switcher) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_switch_callback");
  a->client.switcher = client_switcher;
  return ARCHIVE_OK;
}

int archive_read_set_callback_data(struct archive *_a, void *client_data) {
  return archive_read_set_callback_data2(_a, client_data, 0);
}

int archive_read_set_callback_data2(struct archive *_a, void *client_data,
                                    unsigned int iindex) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_set_callback_data2");

  if (a->client.nodes == 0) {
    a->client.dataset =
        (struct archive_read_data_node *)calloc(1, sizeof(*a->client.dataset));
    if (a->client.dataset == NULL) {
      archive_set_error(&a->archive, ENOMEM, "No memory.");
      return ARCHIVE_FATAL;
    }
    a->client.nodes = 1;
  }

  if (iindex > a->client.nodes - 1) {
    archive_set_error(&a->archive, EINVAL, "Invalid index specified.");
    return ARCHIVE_FATAL;
  }
  a->client.dataset[iindex].data = client_data;
  a->client.dataset[iindex].begin_position = -1;
  a->client.dataset[iindex].total_size = -1;
  return ARCHIVE_OK;
}

int archive_read_add_callback_data(struct archive *_a, void *client_data,
                                   unsigned int iindex) {
  struct archive_read *a = (struct archive_read *)_a;
  void *p;
  unsigned int i;

  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_add_callback_data");
  if (iindex > a->client.nodes) {
    archive_set_error(&a->archive, EINVAL, "Invalid index specified.");
    return ARCHIVE_FATAL;
  }
  p = realloc(a->client.dataset,
              sizeof(*a->client.dataset) * (++(a->client.nodes)));
  if (p == NULL) {
    archive_set_error(&a->archive, ENOMEM, "No memory.");
    return ARCHIVE_FATAL;
  }
  a->client.dataset = (struct archive_read_data_node *)p;
  for (i = a->client.nodes - 1; i > iindex; i--) {
    a->client.dataset[i].data = a->client.dataset[i - 1].data;
    a->client.dataset[i].begin_position = -1;
    a->client.dataset[i].total_size = -1;
  }
  a->client.dataset[iindex].data = client_data;
  a->client.dataset[iindex].begin_position = -1;
  a->client.dataset[iindex].total_size = -1;
  return ARCHIVE_OK;
}

int archive_read_append_callback_data(struct archive *_a, void *client_data) {
  struct archive_read *a = (struct archive_read *)_a;
  return archive_read_add_callback_data(_a, client_data, a->client.nodes);
}

int archive_read_prepend_callback_data(struct archive *_a, void *client_data) {
  return archive_read_add_callback_data(_a, client_data, 0);
}

static const struct archive_read_filter_vtable none_reader_vtable = {
    .read = client_read_proxy,
    .close = client_close_proxy,
};

int archive_read_open1(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  struct archive_read_filter *filter, *tmp;
  int slot, e = ARCHIVE_OK;

  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_read_open");
  archive_clear_error(&a->archive);

  if (a->client.reader == NULL) {
    archive_set_error(&a->archive, EINVAL,
                      "No reader function provided to archive_read_open");
    a->archive.state = ARCHIVE_STATE_FATAL;
    return (ARCHIVE_FATAL);
  }

  if (a->client.opener != NULL) {
    e = (a->client.opener)(&a->archive, a->client.dataset[0].data);
    if (e != 0) {

      read_client_close_proxy(a);
      return (e);
    }
  }

  filter = calloc(1, sizeof(*filter));
  if (filter == NULL)
    return (ARCHIVE_FATAL);
  filter->bidder = NULL;
  filter->upstream = NULL;
  filter->archive = a;
  filter->data = a->client.dataset[0].data;
  filter->vtable = &none_reader_vtable;
  filter->name = "none";
  filter->code = ARCHIVE_FILTER_NONE;
  filter->can_skip = 1;
  filter->can_seek = 1;

  a->client.dataset[0].begin_position = 0;
  if (!a->filter || !a->bypass_filter_bidding) {
    a->filter = filter;

    e = choose_filters(a);
    if (e < ARCHIVE_WARN) {
      a->archive.state = ARCHIVE_STATE_FATAL;
      return (ARCHIVE_FATAL);
    }
  } else {

    tmp = a->filter;
    while (tmp->upstream)
      tmp = tmp->upstream;
    tmp->upstream = filter;
  }

  if (!a->format) {
    slot = choose_format(a);
    if (slot < 0) {
      close_filters(a);
      a->archive.state = ARCHIVE_STATE_FATAL;
      return (ARCHIVE_FATAL);
    }
    a->format = &(a->formats[slot]);
  }

  a->archive.state = ARCHIVE_STATE_HEADER;

  client_switch_proxy(a->filter, 0);
  return (e);
}

#define MAX_NUMBER_FILTERS 25

static int choose_filters(struct archive_read *a) {
  int number_bidders, i, bid, best_bid, number_filters;
  struct archive_read_filter_bidder *bidder, *best_bidder;
  struct archive_read_filter *filter;
  ssize_t avail;
  int r;

  for (number_filters = 0; number_filters < MAX_NUMBER_FILTERS;
       ++number_filters) {
    number_bidders = sizeof(a->bidders) / sizeof(a->bidders[0]);

    best_bid = 0;
    best_bidder = NULL;

    bidder = a->bidders;
    for (i = 0; i < number_bidders; i++, bidder++) {
      if (bidder->vtable == NULL)
        continue;
      bid = (bidder->vtable->bid)(bidder, a->filter);
      if (bid > best_bid) {
        best_bid = bid;
        best_bidder = bidder;
      }
    }

    if (best_bidder == NULL) {

      __archive_read_filter_ahead(a->filter, 1, &avail);
      if (avail < 0) {
        __archive_read_free_filters(a);
        return (ARCHIVE_FATAL);
      }
      return (ARCHIVE_OK);
    }

    filter = calloc(1, sizeof(*filter));
    if (filter == NULL)
      return (ARCHIVE_FATAL);
    filter->bidder = best_bidder;
    filter->archive = a;
    filter->upstream = a->filter;
    a->filter = filter;
    r = (best_bidder->vtable->init)(a->filter);
    if (r != ARCHIVE_OK) {
      __archive_read_free_filters(a);
      return (ARCHIVE_FATAL);
    }
  }
  archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                    "Input requires too many filters for decoding");
  return (ARCHIVE_FATAL);
}

int __archive_read_header(struct archive_read *a, struct archive_entry *entry) {
  if (!a->filter->vtable->read_header)
    return (ARCHIVE_OK);
  return a->filter->vtable->read_header(a->filter, entry);
}

static int _archive_read_next_header2(struct archive *_a,
                                      struct archive_entry *entry) {
  struct archive_read *a = (struct archive_read *)_a;
  int r1 = ARCHIVE_OK, r2;

  archive_check_magic(_a, ARCHIVE_READ_MAGIC,
                      ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
                      "archive_read_next_header");

  archive_entry_clear(entry);
  archive_clear_error(&a->archive);

  if (a->archive.state == ARCHIVE_STATE_DATA) {
    r1 = archive_read_data_skip(&a->archive);
    if (r1 == ARCHIVE_EOF)
      archive_set_error(&a->archive, EIO, "Premature end-of-file.");
    if (r1 == ARCHIVE_EOF || r1 == ARCHIVE_FATAL) {
      a->archive.state = ARCHIVE_STATE_FATAL;
      return (ARCHIVE_FATAL);
    }
  }

  a->header_position = a->filter->position;

  ++_a->file_count;
  r2 = (a->format->read_header)(a, entry);

  switch (r2) {
  case ARCHIVE_EOF:
    a->archive.state = ARCHIVE_STATE_EOF;
    --_a->file_count;
    break;
  case ARCHIVE_OK:
    a->archive.state = ARCHIVE_STATE_DATA;
    break;
  case ARCHIVE_WARN:
    a->archive.state = ARCHIVE_STATE_DATA;
    break;
  case ARCHIVE_RETRY:
    break;
  case ARCHIVE_FATAL:
    a->archive.state = ARCHIVE_STATE_FATAL;
    break;
  }

  __archive_reset_read_data(&a->archive);

  a->data_start_node = a->client.cursor;

  return (r2 < r1 || r2 == ARCHIVE_EOF) ? r2 : r1;
}

static int _archive_read_next_header(struct archive *_a,
                                     struct archive_entry **entryp) {
  int ret;
  struct archive_read *a = (struct archive_read *)_a;
  *entryp = NULL;
  ret = _archive_read_next_header2(_a, a->entry);
  *entryp = a->entry;
  return ret;
}

static int choose_format(struct archive_read *a) {
  int slots;
  int i;
  int bid, best_bid;
  int best_bid_slot;

  slots = sizeof(a->formats) / sizeof(a->formats[0]);
  best_bid = -1;
  best_bid_slot = -1;

  a->format = &(a->formats[0]);
  for (i = 0; i < slots; i++, a->format++) {
    if (a->format->bid) {
      bid = (a->format->bid)(a, best_bid);
      if (bid == ARCHIVE_FATAL)
        return (ARCHIVE_FATAL);
      if (a->filter->position != 0)
        __archive_read_seek(a, 0, SEEK_SET);
      if ((bid > best_bid) || (best_bid_slot < 0)) {
        best_bid = bid;
        best_bid_slot = i;
      }
    }
  }

  if (best_bid_slot < 0) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "No formats registered");
    return (ARCHIVE_FATAL);
  }

  if (best_bid < 1) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Unrecognized archive format");
    return (ARCHIVE_FATAL);
  }

  return (best_bid_slot);
}

la_int64_t archive_read_header_position(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_read_header_position");
  return (a->header_position);
}

int archive_read_has_encrypted_entries(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  int format_supports_encryption = archive_read_format_capabilities(_a) &
                                   (ARCHIVE_READ_FORMAT_CAPS_ENCRYPT_DATA |
                                    ARCHIVE_READ_FORMAT_CAPS_ENCRYPT_METADATA);

  if (!_a || !format_supports_encryption) {

    return ARCHIVE_READ_FORMAT_ENCRYPTION_UNSUPPORTED;
  }

  if (a->format && a->format->has_encrypted_entries) {
    return (a->format->has_encrypted_entries)(a);
  }

  return ARCHIVE_READ_FORMAT_ENCRYPTION_DONT_KNOW;
}

int archive_read_format_capabilities(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  if (a && a->format && a->format->format_capabilties) {
    return (a->format->format_capabilties)(a);
  }
  return ARCHIVE_READ_FORMAT_CAPS_NONE;
}

la_ssize_t archive_read_data(struct archive *_a, void *buff, size_t s) {
  struct archive *a = (struct archive *)_a;
  char *dest;
  const void *read_buf;
  size_t bytes_read;
  size_t len;
  int r;

  bytes_read = 0;
  dest = (char *)buff;

  while (s > 0) {
    if (a->read_data_offset == a->read_data_output_offset &&
        a->read_data_remaining == 0) {
      read_buf = a->read_data_block;
      a->read_data_is_posix_read = 1;
      a->read_data_requested = s;
      r = archive_read_data_block(a, &read_buf, &a->read_data_remaining,
                                  &a->read_data_offset);
      a->read_data_block = read_buf;
      if (r == ARCHIVE_EOF &&
          a->read_data_offset == a->read_data_output_offset &&
          a->read_data_remaining == 0)
        return (bytes_read);

      if (r < ARCHIVE_OK)
        return (r);
    }

    if (a->read_data_offset < a->read_data_output_offset) {
      archive_set_error(a, ARCHIVE_ERRNO_FILE_FORMAT,
                        "Encountered out-of-order sparse blocks");
      return (ARCHIVE_RETRY);
    }

    if (a->read_data_output_offset + (int64_t)s < a->read_data_offset) {
      len = s;
    } else if (a->read_data_output_offset < a->read_data_offset) {
      len = (size_t)(a->read_data_offset - a->read_data_output_offset);
    } else
      len = 0;

    memset(dest, 0, len);
    s -= len;
    a->read_data_output_offset += len;
    dest += len;
    bytes_read += len;

    if (s > 0) {
      len = a->read_data_remaining;
      if (len > s)
        len = s;
      if (len) {
        memcpy(dest, a->read_data_block, len);
        s -= len;
        a->read_data_block += len;
        a->read_data_remaining -= len;
        a->read_data_output_offset += len;
        a->read_data_offset += len;
        dest += len;
        bytes_read += len;
      }
    }
  }
  a->read_data_is_posix_read = 0;
  a->read_data_requested = 0;
  return (bytes_read);
}

void __archive_reset_read_data(struct archive *a) {
  a->read_data_output_offset = 0;
  a->read_data_remaining = 0;
  a->read_data_is_posix_read = 0;
  a->read_data_requested = 0;

  a->read_data_block = NULL;
  a->read_data_offset = 0;
}

int archive_read_data_skip(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  int r;
  const void *buff;
  size_t size;
  int64_t offset;

  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_read_data_skip");

  if (a->format->read_data_skip != NULL)
    r = (a->format->read_data_skip)(a);
  else {
    while ((r = archive_read_data_block(&a->archive, &buff, &size, &offset)) ==
           ARCHIVE_OK)
      ;
  }

  if (r == ARCHIVE_EOF)
    r = ARCHIVE_OK;

  a->archive.state = ARCHIVE_STATE_HEADER;
  return (r);
}

la_int64_t archive_seek_data(struct archive *_a, int64_t offset, int whence) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_seek_data_block");

  if (a->format->seek_data == NULL) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_PROGRAMMER,
                      "Internal error: "
                      "No format_seek_data_block function registered");
    return (ARCHIVE_FATAL);
  }

  return (a->format->seek_data)(a, offset, whence);
}

static int _archive_read_data_block(struct archive *_a, const void **buff,
                                    size_t *size, int64_t *offset) {
  struct archive_read *a = (struct archive_read *)_a;
  archive_check_magic(_a, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_read_data_block");

  if (a->format->read_data == NULL) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_PROGRAMMER,
                      "Internal error: "
                      "No format->read_data function registered");
    return (ARCHIVE_FATAL);
  }

  return (a->format->read_data)(a, buff, size, offset);
}

static int close_filters(struct archive_read *a) {
  struct archive_read_filter *f = a->filter;
  int r = ARCHIVE_OK;

  while (f != NULL) {
    struct archive_read_filter *t = f->upstream;
    if (!f->closed && f->vtable != NULL) {
      int r1 = (f->vtable->close)(f);
      f->closed = 1;
      if (r1 < r)
        r = r1;
    }
    free(f->buffer);
    f->buffer = NULL;
    f = t;
  }
  return r;
}

void __archive_read_free_filters(struct archive_read *a) {

  close_filters(a);

  while (a->filter != NULL) {
    struct archive_read_filter *t = a->filter->upstream;
    free(a->filter);
    a->filter = t;
  }
}

static int _archive_filter_count(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  struct archive_read_filter *p = a->filter;
  int count = 0;
  while (p) {
    count++;
    p = p->upstream;
  }
  return count;
}

static int _archive_read_close(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  int r = ARCHIVE_OK, r1 = ARCHIVE_OK;

  archive_check_magic(&a->archive, ARCHIVE_READ_MAGIC,
                      ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL,
                      "archive_read_close");
  if (a->archive.state == ARCHIVE_STATE_CLOSED)
    return (ARCHIVE_OK);
  archive_clear_error(&a->archive);
  a->archive.state = ARCHIVE_STATE_CLOSED;

  r1 = close_filters(a);
  if (r1 < r)
    r = r1;

  return (r);
}

static int _archive_read_free(struct archive *_a) {
  struct archive_read *a = (struct archive_read *)_a;
  struct archive_read_passphrase *p;
  int i, n;
  int slots;
  int r = ARCHIVE_OK;

  if (_a == NULL)
    return (ARCHIVE_OK);
  archive_check_magic(_a, ARCHIVE_READ_MAGIC,
                      ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL,
                      "archive_read_free");
  if (a->archive.state != ARCHIVE_STATE_CLOSED &&
      a->archive.state != ARCHIVE_STATE_FATAL)
    r = archive_read_close(&a->archive);

  if (a->cleanup_archive_extract != NULL)
    r = (a->cleanup_archive_extract)(a);

  slots = sizeof(a->formats) / sizeof(a->formats[0]);
  for (i = 0; i < slots; i++) {
    a->format = &(a->formats[i]);
    if (a->formats[i].cleanup)
      (a->formats[i].cleanup)(a);
  }

  __archive_read_free_filters(a);

  n = sizeof(a->bidders) / sizeof(a->bidders[0]);
  for (i = 0; i < n; i++) {
    if (a->bidders[i].vtable == NULL || a->bidders[i].vtable->free == NULL)
      continue;
    (a->bidders[i].vtable->free)(&a->bidders[i]);
  }

  p = a->passphrases.first;
  while (p != NULL) {
    struct archive_read_passphrase *np = p->next;

    memset(p->passphrase, 0, strlen(p->passphrase));
    free(p->passphrase);
    free(p);
    p = np;
  }

  archive_string_free(&a->archive.error_string);
  archive_entry_free(a->entry);
  a->archive.magic = 0;
  __archive_clean(&a->archive);
  free(a->client.dataset);
  free(a);
  return (r);
}

static struct archive_read_filter *get_filter(struct archive *_a, int n) {
  struct archive_read *a = (struct archive_read *)_a;
  struct archive_read_filter *f = a->filter;

  if (n == -1 && f != NULL) {
    struct archive_read_filter *last = f;
    f = f->upstream;
    while (f != NULL) {
      last = f;
      f = f->upstream;
    }
    return (last);
  }
  if (n < 0)
    return NULL;
  while (n > 0 && f != NULL) {
    f = f->upstream;
    --n;
  }
  return (f);
}

static int _archive_filter_code(struct archive *_a, int n) {
  struct archive_read_filter *f = get_filter(_a, n);
  return f == NULL ? -1 : f->code;
}

static const char *_archive_filter_name(struct archive *_a, int n) {
  struct archive_read_filter *f = get_filter(_a, n);
  return f != NULL ? f->name : NULL;
}

static int64_t _archive_filter_bytes(struct archive *_a, int n) {
  struct archive_read_filter *f = get_filter(_a, n);
  return f == NULL ? -1 : f->position;
}

int __archive_read_register_format(
    struct archive_read *a, void *format_data, const char *name,
    int (*bid)(struct archive_read *, int),
    int (*options)(struct archive_read *, const char *, const char *),
    int (*read_header)(struct archive_read *, struct archive_entry *),
    int (*read_data)(struct archive_read *, const void **, size_t *, int64_t *),
    int (*read_data_skip)(struct archive_read *),
    int64_t (*seek_data)(struct archive_read *, int64_t, int),
    int (*cleanup)(struct archive_read *),
    int (*format_capabilities)(struct archive_read *),
    int (*has_encrypted_entries)(struct archive_read *)) {
  int i, number_slots;

  archive_check_magic(&a->archive, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "__archive_read_register_format");

  number_slots = sizeof(a->formats) / sizeof(a->formats[0]);

  for (i = 0; i < number_slots; i++) {
    if (a->formats[i].bid == bid)
      return (ARCHIVE_WARN);
    if (a->formats[i].bid == NULL) {
      a->formats[i].bid = bid;
      a->formats[i].options = options;
      a->formats[i].read_header = read_header;
      a->formats[i].read_data = read_data;
      a->formats[i].read_data_skip = read_data_skip;
      a->formats[i].seek_data = seek_data;
      a->formats[i].cleanup = cleanup;
      a->formats[i].data = format_data;
      a->formats[i].name = name;
      a->formats[i].format_capabilties = format_capabilities;
      a->formats[i].has_encrypted_entries = has_encrypted_entries;
      return (ARCHIVE_OK);
    }
  }

  archive_set_error(&a->archive, ENOMEM,
                    "Not enough slots for format registration");
  return (ARCHIVE_FATAL);
}

int __archive_read_register_bidder(
    struct archive_read *a, void *bidder_data, const char *name,
    const struct archive_read_filter_bidder_vtable *vtable) {
  struct archive_read_filter_bidder *bidder;
  int i, number_slots;

  archive_check_magic(&a->archive, ARCHIVE_READ_MAGIC, ARCHIVE_STATE_NEW,
                      "__archive_read_register_bidder");

  number_slots = sizeof(a->bidders) / sizeof(a->bidders[0]);

  for (i = 0; i < number_slots; i++) {
    if (a->bidders[i].vtable != NULL)
      continue;
    memset(a->bidders + i, 0, sizeof(a->bidders[0]));
    bidder = (a->bidders + i);
    bidder->data = bidder_data;
    bidder->name = name;
    bidder->vtable = vtable;
    if (bidder->vtable->bid == NULL || bidder->vtable->init == NULL) {
      archive_set_error(&a->archive, ARCHIVE_ERRNO_PROGRAMMER,
                        "Internal error: "
                        "no bid/init for filter bidder");
      return (ARCHIVE_FATAL);
    }

    return (ARCHIVE_OK);
  }

  archive_set_error(&a->archive, ENOMEM,
                    "Not enough slots for filter registration");
  return (ARCHIVE_FATAL);
}

const void *__archive_read_ahead(struct archive_read *a, size_t min,
                                 ssize_t *avail) {
  return (__archive_read_filter_ahead(a->filter, min, avail));
}

const void *__archive_read_filter_ahead(struct archive_read_filter *filter,
                                        size_t min, ssize_t *avail) {
  ssize_t bytes_read;
  size_t tocopy;

  if (filter->fatal) {
    if (avail)
      *avail = ARCHIVE_FATAL;
    return (NULL);
  }

  for (;;) {

    if (filter->avail >= min && filter->avail > 0) {
      if (avail != NULL)
        *avail = filter->avail;
      return (filter->next);
    }

    if (filter->client_total >= filter->client_avail + filter->avail &&
        filter->client_avail + filter->avail >= min) {

      filter->client_avail += filter->avail;
      filter->client_next -= filter->avail;

      filter->avail = 0;
      filter->next = filter->buffer;

      if (avail != NULL)
        *avail = filter->client_avail;
      return (filter->client_next);
    }

    if (filter->next > filter->buffer &&
        filter->next + min > filter->buffer + filter->buffer_size) {
      if (filter->avail > 0)
        memmove(filter->buffer, filter->next, filter->avail);
      filter->next = filter->buffer;
    }

    if (filter->client_avail <= 0) {
      if (filter->end_of_file) {
        if (avail != NULL)
          *avail = filter->avail;
        return (NULL);
      }
      bytes_read = (filter->vtable->read)(filter, &filter->client_buff);
      if (bytes_read < 0) {
        filter->client_total = filter->client_avail = 0;
        filter->client_next = filter->client_buff = NULL;
        filter->fatal = 1;
        if (avail != NULL)
          *avail = ARCHIVE_FATAL;
        return (NULL);
      }
      if (bytes_read == 0) {

        if (filter->archive->client.cursor !=
            filter->archive->client.nodes - 1) {
          if (client_switch_proxy(filter, filter->archive->client.cursor + 1) ==
              ARCHIVE_OK)
            continue;
        }

        filter->client_total = filter->client_avail = 0;
        filter->client_next = filter->client_buff = NULL;
        filter->end_of_file = 1;

        if (avail != NULL)
          *avail = filter->avail;
        return (NULL);
      }
      filter->client_total = bytes_read;
      filter->client_avail = filter->client_total;
      filter->client_next = filter->client_buff;
    } else {

      if (min > filter->buffer_size) {
        size_t s, t;
        char *p;

        s = t = filter->buffer_size;
        if (s == 0)
          s = min;
        while (s < min) {
          t *= 2;
          if (t <= s) {
            archive_set_error(&filter->archive->archive, ENOMEM,
                              "Unable to allocate copy"
                              " buffer");
            filter->fatal = 1;
            if (avail != NULL)
              *avail = ARCHIVE_FATAL;
            return (NULL);
          }
          s = t;
        }

        p = malloc(s);
        if (p == NULL) {
          archive_set_error(&filter->archive->archive, ENOMEM,
                            "Unable to allocate copy buffer");
          filter->fatal = 1;
          if (avail != NULL)
            *avail = ARCHIVE_FATAL;
          return (NULL);
        }

        if (filter->avail > 0)
          memmove(p, filter->next, filter->avail);
        free(filter->buffer);
        filter->next = filter->buffer = p;
        filter->buffer_size = s;
      }

      tocopy = (filter->buffer + filter->buffer_size) -
               (filter->next + filter->avail);

      if (tocopy + filter->avail > min)
        tocopy = min - filter->avail;

      if (tocopy > filter->client_avail)
        tocopy = filter->client_avail;

      memcpy(filter->next + filter->avail, filter->client_next, tocopy);

      filter->client_next += tocopy;
      filter->client_avail -= tocopy;

      filter->avail += tocopy;
    }
  }
}

int64_t __archive_read_consume(struct archive_read *a, int64_t request) {
  return (__archive_read_filter_consume(a->filter, request));
}

int64_t __archive_read_filter_consume(struct archive_read_filter *filter,
                                      int64_t request) {
  int64_t skipped;

  if (request < 0)
    return ARCHIVE_FATAL;
  if (request == 0)
    return 0;

  skipped = advance_file_pointer(filter, request);
  if (skipped == request)
    return (skipped);

  if (skipped < 0)
    skipped = 0;
  archive_set_error(
      &filter->archive->archive, ARCHIVE_ERRNO_MISC,
      "Truncated input file (needed %jd bytes, only %jd available)",
      (intmax_t)request, (intmax_t)skipped);
  return (ARCHIVE_FATAL);
}

static int64_t advance_file_pointer(struct archive_read_filter *filter,
                                    int64_t request) {
  int64_t bytes_skipped, total_bytes_skipped = 0;
  ssize_t bytes_read;
  size_t min;

  if (filter->fatal)
    return (-1);

  if (filter->avail > 0) {
    min = (size_t)minimum(request, (int64_t)filter->avail);
    filter->next += min;
    filter->avail -= min;
    request -= min;
    filter->position += min;
    total_bytes_skipped += min;
  }

  if (filter->client_avail > 0) {
    min = (size_t)minimum(request, (int64_t)filter->client_avail);
    filter->client_next += min;
    filter->client_avail -= min;
    request -= min;
    filter->position += min;
    total_bytes_skipped += min;
  }
  if (request == 0)
    return (total_bytes_skipped);

  if (filter->can_skip != 0) {
    bytes_skipped = client_skip_proxy(filter, request);
    if (bytes_skipped < 0) {
      filter->fatal = 1;
      return (bytes_skipped);
    }
    filter->position += bytes_skipped;
    total_bytes_skipped += bytes_skipped;
    request -= bytes_skipped;
    if (request == 0)
      return (total_bytes_skipped);
  }

  for (;;) {
    bytes_read = (filter->vtable->read)(filter, &filter->client_buff);
    if (bytes_read < 0) {
      filter->client_buff = NULL;
      filter->fatal = 1;
      return (bytes_read);
    }

    if (bytes_read == 0) {
      if (filter->archive->client.cursor != filter->archive->client.nodes - 1) {
        if (client_switch_proxy(filter, filter->archive->client.cursor + 1) ==
            ARCHIVE_OK)
          continue;
      }
      filter->client_buff = NULL;
      filter->end_of_file = 1;
      return (total_bytes_skipped);
    }

    if (bytes_read >= request) {
      filter->client_next = ((const char *)filter->client_buff) + request;
      filter->client_avail = (size_t)(bytes_read - request);
      filter->client_total = bytes_read;
      total_bytes_skipped += request;
      filter->position += request;
      return (total_bytes_skipped);
    }

    filter->position += bytes_read;
    total_bytes_skipped += bytes_read;
    request -= bytes_read;
  }
}

int64_t __archive_read_seek(struct archive_read *a, int64_t offset,
                            int whence) {
  return __archive_read_filter_seek(a->filter, offset, whence);
}

int64_t __archive_read_filter_seek(struct archive_read_filter *filter,
                                   int64_t offset, int whence) {
  struct archive_read_client *client;
  int64_t r;
  unsigned int cursor;

  if (filter->closed || filter->fatal)
    return (ARCHIVE_FATAL);
  if (filter->can_seek == 0)
    return (ARCHIVE_FAILED);

  client = &(filter->archive->client);
  switch (whence) {
  case SEEK_CUR:

    offset += filter->position;
    __LA_FALLTHROUGH;
  case SEEK_SET:
    cursor = 0;
    while (1) {
      if (client->dataset[cursor].begin_position < 0 ||
          client->dataset[cursor].total_size < 0 ||
          client->dataset[cursor].begin_position +
                  client->dataset[cursor].total_size - 1 >
              offset ||
          cursor + 1 >= client->nodes)
        break;
      r = client->dataset[cursor].begin_position +
          client->dataset[cursor].total_size;
      client->dataset[++cursor].begin_position = r;
    }
    while (1) {
      r = client_switch_proxy(filter, cursor);
      if (r != ARCHIVE_OK)
        return r;
      if ((r = client_seek_proxy(filter, 0, SEEK_END)) < 0)
        return r;
      client->dataset[cursor].total_size = r;
      if (client->dataset[cursor].begin_position +
                  client->dataset[cursor].total_size - 1 >
              offset ||
          cursor + 1 >= client->nodes)
        break;
      r = client->dataset[cursor].begin_position +
          client->dataset[cursor].total_size;
      client->dataset[++cursor].begin_position = r;
    }
    offset -= client->dataset[cursor].begin_position;
    if (offset < 0 || offset > client->dataset[cursor].total_size)
      return ARCHIVE_FATAL;
    if ((r = client_seek_proxy(filter, offset, SEEK_SET)) < 0)
      return r;
    break;

  case SEEK_END:
    cursor = 0;
    while (1) {
      if (client->dataset[cursor].begin_position < 0 ||
          client->dataset[cursor].total_size < 0 || cursor + 1 >= client->nodes)
        break;
      r = client->dataset[cursor].begin_position +
          client->dataset[cursor].total_size;
      client->dataset[++cursor].begin_position = r;
    }
    while (1) {
      r = client_switch_proxy(filter, cursor);
      if (r != ARCHIVE_OK)
        return r;
      if ((r = client_seek_proxy(filter, 0, SEEK_END)) < 0)
        return r;
      client->dataset[cursor].total_size = r;
      r = client->dataset[cursor].begin_position +
          client->dataset[cursor].total_size;
      if (cursor + 1 >= client->nodes)
        break;
      client->dataset[++cursor].begin_position = r;
    }
    while (1) {
      if (r + offset >= client->dataset[cursor].begin_position)
        break;
      offset += client->dataset[cursor].total_size;
      if (cursor == 0)
        break;
      cursor--;
      r = client->dataset[cursor].begin_position +
          client->dataset[cursor].total_size;
    }
    offset = (r + offset) - client->dataset[cursor].begin_position;
    if ((r = client_switch_proxy(filter, cursor)) != ARCHIVE_OK)
      return r;
    r = client_seek_proxy(filter, offset, SEEK_SET);
    if (r < ARCHIVE_OK)
      return r;
    break;

  default:
    return (ARCHIVE_FATAL);
  }
  r += client->dataset[cursor].begin_position;

  if (r >= 0) {

    filter->avail = filter->client_avail = 0;
    filter->next = filter->buffer;
    filter->position = r;
    filter->end_of_file = 0;
  }
  return r;
}

#include "archive_read_private.h"

#include "archive_read_private.h"

#include "archive_read_private.h"
int __archive_read_program(struct archive_read_filter *self, const char *cmd) {
  (void)self;
  (void)cmd;
  return (-1);
}
