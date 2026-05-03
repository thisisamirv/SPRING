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

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif
#include "archive_entry.h"

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#error "archive_entry.h must be included"
#endif

#define ARCHIVE_ENTRY_LINKIFY_LIKE_TAR 0
#define ARCHIVE_ENTRY_LINKIFY_LIKE_MTREE 1
#define ARCHIVE_ENTRY_LINKIFY_LIKE_OLD_CPIO 2
#define ARCHIVE_ENTRY_LINKIFY_LIKE_NEW_CPIO 3

#define links_cache_initial_size 1024

struct links_entry {
  struct links_entry *next;
  struct links_entry *previous;
  struct archive_entry *canonical;
  struct archive_entry *entry;
  size_t hash;
  unsigned int links;
};

struct archive_entry_linkresolver {
  struct links_entry **buckets;
  struct links_entry *spare;
  unsigned long number_entries;
  size_t number_buckets;
  int strategy;
};

#define NEXT_ENTRY_DEFERRED 1
#define NEXT_ENTRY_PARTIAL 2
#define NEXT_ENTRY_ALL (NEXT_ENTRY_DEFERRED | NEXT_ENTRY_PARTIAL)

static struct links_entry *find_entry(struct archive_entry_linkresolver *,
                                      struct archive_entry *);
static void grow_hash(struct archive_entry_linkresolver *);
static struct links_entry *insert_entry(struct archive_entry_linkresolver *,
                                        struct archive_entry *);
static struct links_entry *next_entry(struct archive_entry_linkresolver *, int);

struct archive_entry_linkresolver *archive_entry_linkresolver_new(void) {
  struct archive_entry_linkresolver *res;

  if (links_cache_initial_size == 0 ||
      (links_cache_initial_size & (links_cache_initial_size - 1)) != 0)
    return (NULL);

  res = calloc(1, sizeof(struct archive_entry_linkresolver));
  if (res == NULL)
    return (NULL);
  res->number_buckets = links_cache_initial_size;
  res->buckets = calloc(res->number_buckets, sizeof(res->buckets[0]));
  if (res->buckets == NULL) {
    free(res);
    return (NULL);
  }
  return (res);
}

void archive_entry_linkresolver_set_strategy(
    struct archive_entry_linkresolver *res, int fmt) {
  int fmtbase = fmt & ARCHIVE_FORMAT_BASE_MASK;

  switch (fmtbase) {
  case ARCHIVE_FORMAT_7ZIP:
  case ARCHIVE_FORMAT_AR:
  case ARCHIVE_FORMAT_ZIP:
    res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_OLD_CPIO;
    break;
  case ARCHIVE_FORMAT_CPIO:
    switch (fmt) {
    case ARCHIVE_FORMAT_CPIO_SVR4_NOCRC:
    case ARCHIVE_FORMAT_CPIO_SVR4_CRC:
      res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_NEW_CPIO;
      break;
    default:
      res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_OLD_CPIO;
      break;
    }
    break;
  case ARCHIVE_FORMAT_MTREE:
    res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_MTREE;
    break;
  case ARCHIVE_FORMAT_ISO9660:
  case ARCHIVE_FORMAT_SHAR:
  case ARCHIVE_FORMAT_TAR:
  case ARCHIVE_FORMAT_XAR:
    res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_TAR;
    break;
  default:
    res->strategy = ARCHIVE_ENTRY_LINKIFY_LIKE_OLD_CPIO;
    break;
  }
}

void archive_entry_linkresolver_free(struct archive_entry_linkresolver *res) {
  struct links_entry *le;

  if (res == NULL)
    return;

  while ((le = next_entry(res, NEXT_ENTRY_ALL)) != NULL)
    archive_entry_free(le->entry);
  free(res->buckets);
  free(res);
}

void archive_entry_linkify(struct archive_entry_linkresolver *res,
                           struct archive_entry **e, struct archive_entry **f) {
  struct links_entry *le;
  struct archive_entry *t;

  *f = NULL;

  if (*e == NULL) {
    le = next_entry(res, NEXT_ENTRY_DEFERRED);
    if (le != NULL) {
      *e = le->entry;
      le->entry = NULL;
    }
    return;
  }

  if (archive_entry_nlink(*e) == 1)
    return;

  if (archive_entry_filetype(*e) == AE_IFDIR ||
      archive_entry_filetype(*e) == AE_IFBLK ||
      archive_entry_filetype(*e) == AE_IFCHR)
    return;

  switch (res->strategy) {
  case ARCHIVE_ENTRY_LINKIFY_LIKE_TAR:
    le = find_entry(res, *e);
    if (le != NULL) {
      archive_entry_unset_size(*e);
#if defined(_WIN32) && !defined(__CYGWIN__)
      archive_entry_copy_hardlink_w(*e,
                                    archive_entry_pathname_w(le->canonical));
#else
      archive_entry_copy_hardlink(*e, archive_entry_pathname(le->canonical));
#endif
    } else
      insert_entry(res, *e);
    return;
  case ARCHIVE_ENTRY_LINKIFY_LIKE_MTREE:
    le = find_entry(res, *e);
    if (le != NULL) {
#if defined(_WIN32) && !defined(__CYGWIN__)
      archive_entry_copy_hardlink_w(*e,
                                    archive_entry_pathname_w(le->canonical));
#else
      archive_entry_copy_hardlink(*e, archive_entry_pathname(le->canonical));
#endif
    } else
      insert_entry(res, *e);
    return;
  case ARCHIVE_ENTRY_LINKIFY_LIKE_OLD_CPIO:

    return;
  case ARCHIVE_ENTRY_LINKIFY_LIKE_NEW_CPIO:
    le = find_entry(res, *e);
    if (le != NULL) {

      t = *e;
      *e = le->entry;
      le->entry = t;

      archive_entry_unset_size(*e);
#if defined(_WIN32) && !defined(__CYGWIN__)
      archive_entry_copy_hardlink_w(*e,
                                    archive_entry_pathname_w(le->canonical));
#else
      archive_entry_copy_hardlink(*e, archive_entry_pathname(le->canonical));
#endif

      if (le->links == 0) {
        *f = le->entry;
        le->entry = NULL;
      }
    } else {

      le = insert_entry(res, *e);
      if (le == NULL)

        return;
      le->entry = *e;
      *e = NULL;
    }
    return;
  default:
    break;
  }
  return;
}

static struct links_entry *find_entry(struct archive_entry_linkresolver *res,
                                      struct archive_entry *entry) {
  struct links_entry *le;
  size_t hash, bucket;
  dev_t dev;
  int64_t ino;

  if (!archive_entry_ino_is_set(entry) || !archive_entry_dev_is_set(entry)) {
    return (NULL);
  }

  if (res->spare != NULL) {
    archive_entry_free(res->spare->canonical);
    archive_entry_free(res->spare->entry);
    free(res->spare);
    res->spare = NULL;
  }

  dev = archive_entry_dev(entry);
  ino = archive_entry_ino64(entry);
  hash = (size_t)(dev ^ ino);

  bucket = hash & (res->number_buckets - 1);
  for (le = res->buckets[bucket]; le != NULL; le = le->next) {
    if (le->hash == hash && dev == archive_entry_dev(le->canonical) &&
        ino == archive_entry_ino64(le->canonical)) {

      --le->links;
      if (le->links > 0)
        return (le);

      if (le->previous != NULL)
        le->previous->next = le->next;
      if (le->next != NULL)
        le->next->previous = le->previous;
      if (res->buckets[bucket] == le)
        res->buckets[bucket] = le->next;
      res->number_entries--;

      res->spare = le;
      return (le);
    }
  }
  return (NULL);
}

static struct links_entry *next_entry(struct archive_entry_linkresolver *res,
                                      int mode) {
  struct links_entry *le;
  size_t bucket;

  if (res->spare != NULL) {
    archive_entry_free(res->spare->canonical);
    archive_entry_free(res->spare->entry);
    free(res->spare);
    res->spare = NULL;
  }

  for (bucket = 0; bucket < res->number_buckets; bucket++) {
    for (le = res->buckets[bucket]; le != NULL; le = le->next) {
      if (le->entry != NULL && (mode & NEXT_ENTRY_DEFERRED) == 0)
        continue;
      if (le->entry == NULL && (mode & NEXT_ENTRY_PARTIAL) == 0)
        continue;

      if (le->next != NULL)
        le->next->previous = le->previous;
      if (le->previous != NULL)
        le->previous->next = le->next;
      else
        res->buckets[bucket] = le->next;
      res->number_entries--;

      res->spare = le;
      return (le);
    }
  }
  return (NULL);
}

static struct links_entry *insert_entry(struct archive_entry_linkresolver *res,
                                        struct archive_entry *entry) {
  struct links_entry *le;
  size_t hash, bucket;

  if (!archive_entry_ino_is_set(entry) || !archive_entry_dev_is_set(entry)) {
    return (NULL);
  }

  le = calloc(1, sizeof(struct links_entry));
  if (le == NULL)
    return (NULL);
  le->canonical = archive_entry_clone(entry);

  if (res->number_entries > res->number_buckets * 2)
    grow_hash(res);

  hash = (size_t)(archive_entry_dev(entry) ^ archive_entry_ino64(entry));
  bucket = hash & (res->number_buckets - 1);

  if (res->buckets[bucket] != NULL)
    res->buckets[bucket]->previous = le;
  res->number_entries++;
  le->next = res->buckets[bucket];
  le->previous = NULL;
  res->buckets[bucket] = le;
  le->hash = hash;
  le->links = archive_entry_nlink(entry) - 1;
  return (le);
}

static void grow_hash(struct archive_entry_linkresolver *res) {
  struct links_entry *le, **new_buckets;
  size_t new_size;
  size_t i, bucket;

  new_size = res->number_buckets * 2;
  if (new_size < res->number_buckets)
    return;
  new_buckets = calloc(new_size, sizeof(struct links_entry *));

  if (new_buckets == NULL)
    return;

  for (i = 0; i < res->number_buckets; i++) {
    while (res->buckets[i] != NULL) {

      le = res->buckets[i];
      res->buckets[i] = le->next;

      bucket = le->hash & (new_size - 1);

      if (new_buckets[bucket] != NULL)
        new_buckets[bucket]->previous = le;
      le->next = new_buckets[bucket];
      le->previous = NULL;
      new_buckets[bucket] = le;
    }
  }
  free(res->buckets);
  res->buckets = new_buckets;
  res->number_buckets = new_size;
}

struct archive_entry *
archive_entry_partial_links(struct archive_entry_linkresolver *res,
                            unsigned int *links) {
  struct archive_entry *e;
  struct links_entry *le;

  if (res->spare != NULL) {
    archive_entry_free(res->spare->canonical);
    archive_entry_free(res->spare->entry);
    free(res->spare);
    res->spare = NULL;
  }

  le = next_entry(res, NEXT_ENTRY_PARTIAL);
  if (le != NULL) {
    e = le->canonical;
    if (links != NULL)
      *links = le->links;
    le->canonical = NULL;
  } else {
    e = NULL;
    if (links != NULL)
      *links = 0;
  }
  return (e);
}
