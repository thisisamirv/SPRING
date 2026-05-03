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
 * Based on: NetBSD: rb.c,v 1.6 2010/04/30 13:58:09 joerg Exp
 */

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#include <stddef.h>

#include "archive_rb.h"

#define RB_DIR_LEFT 0
#define RB_DIR_RIGHT 1
#define RB_DIR_OTHER 1
#define rb_left rb_nodes[RB_DIR_LEFT]
#define rb_right rb_nodes[RB_DIR_RIGHT]

#define RB_FLAG_POSITION 0x2
#define RB_FLAG_RED 0x1
#define RB_FLAG_MASK (RB_FLAG_POSITION | RB_FLAG_RED)
#define RB_FATHER(rb)                                                          \
  ((struct archive_rb_node *)((rb)->rb_info & ~RB_FLAG_MASK))
#define RB_SET_FATHER(rb, father)                                              \
  ((void)((rb)->rb_info = (uintptr_t)(father) | ((rb)->rb_info & RB_FLAG_MASK)))

#define RB_SENTINEL_P(rb) ((rb) == NULL)
#define RB_LEFT_SENTINEL_P(rb) RB_SENTINEL_P((rb)->rb_left)
#define RB_RIGHT_SENTINEL_P(rb) RB_SENTINEL_P((rb)->rb_right)
#define RB_FATHER_SENTINEL_P(rb) RB_SENTINEL_P(RB_FATHER((rb)))
#define RB_CHILDLESS_P(rb)                                                     \
  (RB_SENTINEL_P(rb) || (RB_LEFT_SENTINEL_P(rb) && RB_RIGHT_SENTINEL_P(rb)))
#define RB_TWOCHILDREN_P(rb)                                                   \
  (!RB_SENTINEL_P(rb) && !RB_LEFT_SENTINEL_P(rb) && !RB_RIGHT_SENTINEL_P(rb))

#define RB_POSITION(rb)                                                        \
  (((rb)->rb_info & RB_FLAG_POSITION) ? RB_DIR_RIGHT : RB_DIR_LEFT)
#define RB_RIGHT_P(rb) (RB_POSITION(rb) == RB_DIR_RIGHT)
#define RB_LEFT_P(rb) (RB_POSITION(rb) == RB_DIR_LEFT)
#define RB_RED_P(rb) (!RB_SENTINEL_P(rb) && ((rb)->rb_info & RB_FLAG_RED) != 0)
#define RB_BLACK_P(rb) (RB_SENTINEL_P(rb) || ((rb)->rb_info & RB_FLAG_RED) == 0)
#define RB_MARK_RED(rb) ((void)((rb)->rb_info |= RB_FLAG_RED))
#define RB_MARK_BLACK(rb) ((void)((rb)->rb_info &= ~RB_FLAG_RED))
#define RB_INVERT_COLOR(rb) ((void)((rb)->rb_info ^= RB_FLAG_RED))
#define RB_ROOT_P(rbt, rb) ((rbt)->rbt_root == (rb))
#define RB_SET_POSITION(rb, position)                                          \
  ((void)((position) ? ((rb)->rb_info |= RB_FLAG_POSITION)                     \
                     : ((rb)->rb_info &= ~RB_FLAG_POSITION)))
#define RB_ZERO_PROPERTIES(rb) ((void)((rb)->rb_info &= ~RB_FLAG_MASK))
#define RB_COPY_PROPERTIES(dst, src)                                           \
  ((void)((dst)->rb_info ^= ((dst)->rb_info ^ (src)->rb_info) & RB_FLAG_MASK))
#define RB_SWAP_PROPERTIES(a, b)                                               \
  do {                                                                         \
    uintptr_t xorinfo = ((a)->rb_info ^ (b)->rb_info) & RB_FLAG_MASK;          \
    (a)->rb_info ^= xorinfo;                                                   \
    (b)->rb_info ^= xorinfo;                                                   \
  } while (0)

static void __archive_rb_tree_insert_rebalance(struct archive_rb_tree *,
                                               struct archive_rb_node *);
static void __archive_rb_tree_removal_rebalance(struct archive_rb_tree *,
                                                struct archive_rb_node *,
                                                unsigned int);

#define RB_SENTINEL_NODE NULL

#define T 1
#define F 0

void __archive_rb_tree_init(struct archive_rb_tree *rbt,
                            const struct archive_rb_tree_ops *ops) {
  rbt->rbt_ops = ops;
  *((struct archive_rb_node **)&rbt->rbt_root) = RB_SENTINEL_NODE;
}

struct archive_rb_node *__archive_rb_tree_find_node(struct archive_rb_tree *rbt,
                                                    const void *key) {
  archive_rbto_compare_key_fn compare_key = rbt->rbt_ops->rbto_compare_key;
  struct archive_rb_node *parent = rbt->rbt_root;

  while (!RB_SENTINEL_P(parent)) {
    const signed int diff = (*compare_key)(parent, key);
    if (diff == 0)
      return parent;
    parent = parent->rb_nodes[diff > 0];
  }

  return NULL;
}

struct archive_rb_node *
__archive_rb_tree_find_node_geq(struct archive_rb_tree *rbt, const void *key) {
  archive_rbto_compare_key_fn compare_key = rbt->rbt_ops->rbto_compare_key;
  struct archive_rb_node *parent = rbt->rbt_root;
  struct archive_rb_node *last = NULL;

  while (!RB_SENTINEL_P(parent)) {
    const signed int diff = (*compare_key)(parent, key);
    if (diff == 0)
      return parent;
    if (diff < 0)
      last = parent;
    parent = parent->rb_nodes[diff > 0];
  }

  return last;
}

struct archive_rb_node *
__archive_rb_tree_find_node_leq(struct archive_rb_tree *rbt, const void *key) {
  archive_rbto_compare_key_fn compare_key = rbt->rbt_ops->rbto_compare_key;
  struct archive_rb_node *parent = rbt->rbt_root;
  struct archive_rb_node *last = NULL;

  while (!RB_SENTINEL_P(parent)) {
    const signed int diff = (*compare_key)(parent, key);
    if (diff == 0)
      return parent;
    if (diff > 0)
      last = parent;
    parent = parent->rb_nodes[diff > 0];
  }

  return last;
}

int __archive_rb_tree_insert_node(struct archive_rb_tree *rbt,
                                  struct archive_rb_node *self) {
  archive_rbto_compare_nodes_fn compare_nodes =
      rbt->rbt_ops->rbto_compare_nodes;
  struct archive_rb_node *parent, *tmp;
  unsigned int position;
  int rebalance;

  tmp = rbt->rbt_root;

  parent = (struct archive_rb_node *)(void *)&rbt->rbt_root;
  position = RB_DIR_LEFT;

  while (!RB_SENTINEL_P(tmp)) {
    const signed int diff = (*compare_nodes)(tmp, self);
    if (diff == 0) {

      return F;
    }
    parent = tmp;
    position = (diff > 0);
    tmp = parent->rb_nodes[position];
  }

  RB_SET_FATHER(self, parent);
  RB_SET_POSITION(self, position);
  if (parent == (struct archive_rb_node *)(void *)&rbt->rbt_root) {
    RB_MARK_BLACK(self);
    rebalance = F;
  } else {

    RB_MARK_RED(self);
    rebalance = RB_RED_P(parent);
  }
  self->rb_left = parent->rb_nodes[position];
  self->rb_right = parent->rb_nodes[position];
  parent->rb_nodes[position] = self;

  if (rebalance)
    __archive_rb_tree_insert_rebalance(rbt, self);

  return T;
}

static void __archive_rb_tree_reparent_nodes(struct archive_rb_node *old_father,
                                             const unsigned int which) {
  const unsigned int other = which ^ RB_DIR_OTHER;
  struct archive_rb_node *const grandpa = RB_FATHER(old_father);
  struct archive_rb_node *const old_child = old_father->rb_nodes[which];
  struct archive_rb_node *const new_father = old_child;
  struct archive_rb_node *const new_child = old_father;

  if (new_father == NULL)
    return;

  grandpa->rb_nodes[RB_POSITION(old_father)] = new_father;
  new_child->rb_nodes[which] = old_child->rb_nodes[other];
  new_father->rb_nodes[other] = new_child;

  RB_SET_FATHER(new_father, grandpa);
  RB_SET_FATHER(new_child, new_father);

  RB_SWAP_PROPERTIES(new_father, new_child);
  RB_SET_POSITION(new_child, other);

  if (!RB_SENTINEL_P(new_child->rb_nodes[which])) {
    RB_SET_FATHER(new_child->rb_nodes[which], new_child);
    RB_SET_POSITION(new_child->rb_nodes[which], which);
  }
}

static void __archive_rb_tree_insert_rebalance(struct archive_rb_tree *rbt,
                                               struct archive_rb_node *self) {
  struct archive_rb_node *father = RB_FATHER(self);
  struct archive_rb_node *grandpa;
  struct archive_rb_node *uncle;
  unsigned int which;
  unsigned int other;

  for (;;) {

    grandpa = RB_FATHER(father);
    which = (father == grandpa->rb_right);
    other = which ^ RB_DIR_OTHER;
    uncle = grandpa->rb_nodes[other];

    if (RB_BLACK_P(uncle))
      break;

    RB_MARK_BLACK(uncle);
    RB_MARK_BLACK(father);
    if (RB_ROOT_P(rbt, grandpa)) {

      return;
    }
    RB_MARK_RED(grandpa);
    self = grandpa;
    father = RB_FATHER(self);
    if (RB_BLACK_P(father)) {

      return;
    }
  }

  if (self == father->rb_nodes[other]) {

    __archive_rb_tree_reparent_nodes(father, other);
  }

  __archive_rb_tree_reparent_nodes(grandpa, which);

  RB_MARK_BLACK(rbt->rbt_root);
}

static void __archive_rb_tree_prune_node(struct archive_rb_tree *rbt,
                                         struct archive_rb_node *self,
                                         int rebalance) {
  const unsigned int which = RB_POSITION(self);
  struct archive_rb_node *father = RB_FATHER(self);

  father->rb_nodes[which] = self->rb_left;

  if (rebalance)
    __archive_rb_tree_removal_rebalance(rbt, father, which);
}

static void
__archive_rb_tree_swap_prune_and_rebalance(struct archive_rb_tree *rbt,
                                           struct archive_rb_node *self,
                                           struct archive_rb_node *standin) {
  const unsigned int standin_which = RB_POSITION(standin);
  unsigned int standin_other = standin_which ^ RB_DIR_OTHER;
  struct archive_rb_node *standin_son;
  struct archive_rb_node *standin_father = RB_FATHER(standin);
  int rebalance = RB_BLACK_P(standin);

  if (standin_father == self) {

    standin_son = standin->rb_nodes[standin_which];
  } else {

    standin_son = standin->rb_nodes[standin_other];
  }

  if (RB_RED_P(standin_son)) {

    RB_MARK_BLACK(standin_son);
    rebalance = F;

    if (standin_father != self) {

      RB_SET_FATHER(standin_son, standin_father);
      RB_SET_POSITION(standin_son, standin_which);
    }
  }

  if (standin_father == self) {

    standin_father = standin;
  } else {

    standin_father->rb_nodes[standin_which] = standin_son;

    standin->rb_nodes[standin_other] = self->rb_nodes[standin_other];
    RB_SET_FATHER(standin->rb_nodes[standin_other], standin);

    standin_other = standin_which;
  }

  standin->rb_nodes[standin_other] = self->rb_nodes[standin_other];
  RB_SET_FATHER(standin->rb_nodes[standin_other], standin);

  RB_COPY_PROPERTIES(standin, self);
  RB_SET_FATHER(standin, RB_FATHER(self));
  RB_FATHER(standin)->rb_nodes[RB_POSITION(standin)] = standin;

  if (rebalance)
    __archive_rb_tree_removal_rebalance(rbt, standin_father, standin_which);
}

static void
__archive_rb_tree_prune_blackred_branch(struct archive_rb_node *self,
                                        unsigned int which) {
  struct archive_rb_node *father = RB_FATHER(self);
  struct archive_rb_node *son = self->rb_nodes[which];

  RB_COPY_PROPERTIES(son, self);
  father->rb_nodes[RB_POSITION(son)] = son;
  RB_SET_FATHER(son, father);
}

void __archive_rb_tree_remove_node(struct archive_rb_tree *rbt,
                                   struct archive_rb_node *self) {
  struct archive_rb_node *standin;
  unsigned int which;

  if (RB_CHILDLESS_P(self)) {
    const int rebalance = RB_BLACK_P(self) && !RB_ROOT_P(rbt, self);
    __archive_rb_tree_prune_node(rbt, self, rebalance);
    return;
  }
  if (!RB_TWOCHILDREN_P(self)) {

    which = RB_LEFT_SENTINEL_P(self) ? RB_DIR_RIGHT : RB_DIR_LEFT;
    __archive_rb_tree_prune_blackred_branch(self, which);
    return;
  }

  which = RB_POSITION(self) ^ RB_DIR_OTHER;

  standin = __archive_rb_tree_iterate(rbt, self, which);
  __archive_rb_tree_swap_prune_and_rebalance(rbt, self, standin);
}

static void __archive_rb_tree_removal_rebalance(struct archive_rb_tree *rbt,
                                                struct archive_rb_node *parent,
                                                unsigned int which) {

  while (RB_BLACK_P(parent->rb_nodes[which])) {
    unsigned int other = which ^ RB_DIR_OTHER;
    struct archive_rb_node *brother = parent->rb_nodes[other];

    if (brother == NULL)
      return;

    if (RB_BLACK_P(parent) && RB_BLACK_P(brother->rb_left) &&
        RB_BLACK_P(brother->rb_right)) {
      if (RB_RED_P(brother)) {

        __archive_rb_tree_reparent_nodes(parent, other);
        brother = parent->rb_nodes[other];
        if (brother == NULL)
          return;
      } else {

        RB_MARK_RED(brother);
        if (RB_ROOT_P(rbt, parent))
          return;
        which = RB_POSITION(parent);
        parent = RB_FATHER(parent);
        continue;
      }
    }

    if (RB_RED_P(parent) && RB_BLACK_P(brother) &&
        RB_BLACK_P(brother->rb_left) && RB_BLACK_P(brother->rb_right)) {

      RB_MARK_BLACK(parent);
      RB_MARK_RED(brother);
      break;
    } else {

      if (RB_BLACK_P(brother->rb_nodes[other])) {

        __archive_rb_tree_reparent_nodes(brother, which);
        brother = parent->rb_nodes[other];
      }

      if (brother->rb_nodes[other] == NULL)
        return;
      RB_MARK_BLACK(brother->rb_nodes[other]);
      __archive_rb_tree_reparent_nodes(parent, other);
      break;
    }
  }
}

struct archive_rb_node *
__archive_rb_tree_iterate(struct archive_rb_tree *rbt,
                          struct archive_rb_node *self,
                          const unsigned int direction) {
  const unsigned int other = direction ^ RB_DIR_OTHER;

  if (self == NULL) {
    self = rbt->rbt_root;
    if (RB_SENTINEL_P(self))
      return NULL;
    while (!RB_SENTINEL_P(self->rb_nodes[direction]))
      self = self->rb_nodes[direction];
    return self;
  }

  if (RB_SENTINEL_P(self->rb_nodes[direction])) {
    while (!RB_ROOT_P(rbt, self)) {
      if (other == (unsigned int)RB_POSITION(self))
        return RB_FATHER(self);
      self = RB_FATHER(self);
    }
    return NULL;
  }

  self = self->rb_nodes[direction];
  while (!RB_SENTINEL_P(self->rb_nodes[other]))
    self = self->rb_nodes[other];
  return self;
}
