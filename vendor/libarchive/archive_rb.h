/*-
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Matt Thomas <matt@3am-software.com>.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Based on NetBSD: rb.h,v 1.13 2009/08/16 10:57:01 yamt Exp
 */

#ifndef ARCHIVE_RB_H_INCLUDED
#define ARCHIVE_RB_H_INCLUDED

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

struct archive_rb_node {
  struct archive_rb_node *rb_nodes[2];

  uintptr_t rb_info;
};

#define ARCHIVE_RB_DIR_LEFT 0
#define ARCHIVE_RB_DIR_RIGHT 1

#define ARCHIVE_RB_TREE_MIN(T)                                                 \
  __archive_rb_tree_iterate((T), NULL, ARCHIVE_RB_DIR_LEFT)
#define ARCHIVE_RB_TREE_MAX(T)                                                 \
  __archive_rb_tree_iterate((T), NULL, ARCHIVE_RB_DIR_RIGHT)
#define ARCHIVE_RB_TREE_NEXT(T, N)                                             \
  __archive_rb_tree_iterate((T), (N), ARCHIVE_RB_DIR_RIGHT)
#define ARCHIVE_RB_TREE_PREV(T, N)                                             \
  __archive_rb_tree_iterate((T), (N), ARCHIVE_RB_DIR_LEFT)
#define ARCHIVE_RB_TREE_FOREACH(N, T)                                          \
  for ((N) = ARCHIVE_RB_TREE_MIN(T); (N); (N) = ARCHIVE_RB_TREE_NEXT((T), (N)))
#define ARCHIVE_RB_TREE_FOREACH_REVERSE(N, T)                                  \
  for ((N) = ARCHIVE_RB_TREE_MAX(T); (N); (N) = ARCHIVE_RB_TREE_PREV((T), (N)))
#define ARCHIVE_RB_TREE_FOREACH_SAFE(N, T, S)                                  \
  for ((N) = ARCHIVE_RB_TREE_MIN(T);                                           \
       (N) && ((S) = ARCHIVE_RB_TREE_NEXT((T), (N)), 1); (N) = (S))
#define ARCHIVE_RB_TREE_FOREACH_REVERSE_SAFE(N, T, S)                          \
  for ((N) = ARCHIVE_RB_TREE_MAX(T);                                           \
       (N) && ((S) = ARCHIVE_RB_TREE_PREV((T), (N)), 1); (N) = (S))

typedef signed int (*const archive_rbto_compare_nodes_fn)(
    const struct archive_rb_node *, const struct archive_rb_node *);
typedef signed int (*const archive_rbto_compare_key_fn)(
    const struct archive_rb_node *, const void *);

struct archive_rb_tree_ops {
  archive_rbto_compare_nodes_fn rbto_compare_nodes;
  archive_rbto_compare_key_fn rbto_compare_key;
};

struct archive_rb_tree {
  struct archive_rb_node *rbt_root;
  const struct archive_rb_tree_ops *rbt_ops;
};

void __archive_rb_tree_init(struct archive_rb_tree *,
                            const struct archive_rb_tree_ops *);
int __archive_rb_tree_insert_node(struct archive_rb_tree *,
                                  struct archive_rb_node *);
struct archive_rb_node *__archive_rb_tree_find_node(struct archive_rb_tree *,
                                                    const void *);
struct archive_rb_node *
__archive_rb_tree_find_node_geq(struct archive_rb_tree *, const void *);
struct archive_rb_node *
__archive_rb_tree_find_node_leq(struct archive_rb_tree *, const void *);
void __archive_rb_tree_remove_node(struct archive_rb_tree *,
                                   struct archive_rb_node *);
struct archive_rb_node *__archive_rb_tree_iterate(struct archive_rb_tree *,
                                                  struct archive_rb_node *,
                                                  const unsigned int);

#endif
