/* inffast_chunk.c -- fast decoding
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
 * Copyright (C) 1995-2017 Mark Adler
 * Copyright 2023 The Chromium Authors
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "inffast_chunk.h"
#include "chunkcopy.h"
#include "inffast.h"
#include "inflate.h"
#include "inftrees.h"
#include "zutil.h"

#ifdef ASMINF
#pragma message("Assembler code may have bugs -- use at your own risk")
#else

void ZLIB_INTERNAL inflate_fast_chunk_(z_streamp strm, unsigned start) {
  struct inflate_state FAR *state;
  z_const unsigned char FAR *in;
  z_const unsigned char FAR *last;
  unsigned char FAR *out;
  unsigned char FAR *beg;
  unsigned char FAR *end;
  unsigned char FAR *limit;
#ifdef INFLATE_STRICT
  unsigned dmax;
#endif
  unsigned wsize;
  unsigned whave;
  unsigned wnext;
  unsigned char FAR *window;
  inflate_holder_t hold;
  unsigned bits;
  code const FAR *lcode;
  code const FAR *dcode;
  unsigned lmask;
  unsigned dmask;
  code const *here;
  unsigned op;

  unsigned len;
  unsigned dist;
  unsigned char FAR *from;

  state = (struct inflate_state FAR *)strm->state;
  in = strm->next_in;
  last = in + (strm->avail_in - (INFLATE_FAST_MIN_INPUT - 1));
  out = strm->next_out;
  beg = out - (start - strm->avail_out);
  end = out + (strm->avail_out - (INFLATE_FAST_MIN_OUTPUT - 1));
  limit = out + strm->avail_out;
#ifdef INFLATE_STRICT
  dmax = state->dmax;
#endif
  wsize = state->wsize;
  whave = state->whave;
  wnext = (state->wnext == 0 && whave >= wsize) ? wsize : state->wnext;
  window = state->window;
  hold = state->hold;
  bits = state->bits;
  lcode = state->lencode;
  dcode = state->distcode;
  lmask = (1U << state->lenbits) - 1;
  dmask = (1U << state->distbits) - 1;

#ifdef INFLATE_CHUNK_READ_64LE
#define REFILL()                                                               \
  do {                                                                         \
    Assert(bits < 64, "### Too many bits in inflate_fast.");                   \
    hold |= read64le(in) << bits;                                              \
    in += 7;                                                                   \
    in -= bits >> 3;                                                           \
    bits |= 56;                                                                \
  } while (0)
#endif

  do {
#ifdef INFLATE_CHUNK_READ_64LE
    REFILL();
#else
    if (bits < 15) {
      hold += (unsigned long)(*in++) << bits;
      bits += 8;
      hold += (unsigned long)(*in++) << bits;
      bits += 8;
    }
#endif
    here = lcode + (hold & lmask);
#ifdef INFLATE_CHUNK_READ_64LE
    if (here->op == 0) {
      Tracevv((stderr,
               here->val >= 0x20 && here->val < 0x7f
                   ? "inflate:         literal '%c'\n"
                   : "inflate:         literal 0x%02x\n",
               here->val));
      *out++ = (unsigned char)(here->val);
      hold >>= here->bits;
      bits -= here->bits;
      here = lcode + (hold & lmask);
      if (here->op == 0) {
        Tracevv((stderr,
                 here->val >= 0x20 && here->val < 0x7f
                     ? "inflate:    2nd  literal '%c'\n"
                     : "inflate:    2nd  literal 0x%02x\n",
                 here->val));
        *out++ = (unsigned char)(here->val);
        hold >>= here->bits;
        bits -= here->bits;
        here = lcode + (hold & lmask);
      }
    }
#endif
  dolen:
    op = (unsigned)(here->bits);
    hold >>= op;
    bits -= op;
    op = (unsigned)(here->op);
    if (op == 0) {
      Tracevv((stderr,
               here->val >= 0x20 && here->val < 0x7f
                   ? "inflate:         literal '%c'\n"
                   : "inflate:         literal 0x%02x\n",
               here->val));
      *out++ = (unsigned char)(here->val);
    } else if (op & 16) {
      len = (unsigned)(here->val);
      op &= 15;
      if (op) {
#ifndef INFLATE_CHUNK_READ_64LE
        if (bits < op) {
          hold += (unsigned long)(*in++) << bits;
          bits += 8;
        }
#endif
        len += (unsigned)hold & ((1U << op) - 1);
        hold >>= op;
        bits -= op;
      }
      Tracevv((stderr, "inflate:         length %u\n", len));
#ifndef INFLATE_CHUNK_READ_64LE
      if (bits < 15) {
        hold += (unsigned long)(*in++) << bits;
        bits += 8;
        hold += (unsigned long)(*in++) << bits;
        bits += 8;
      }
#endif
      here = dcode + (hold & dmask);
    dodist:
      op = (unsigned)(here->bits);
      hold >>= op;
      bits -= op;
      op = (unsigned)(here->op);
      if (op & 16) {
        dist = (unsigned)(here->val);
        op &= 15;

        if (bits < op) {
#ifdef INFLATE_CHUNK_READ_64LE
          REFILL();
#else
          hold += (unsigned long)(*in++) << bits;
          bits += 8;
          if (bits < op) {
            hold += (unsigned long)(*in++) << bits;
            bits += 8;
          }
#endif
        }
        dist += (unsigned)hold & ((1U << op) - 1);
#ifdef INFLATE_STRICT
        if (dist > dmax) {
          strm->msg = (char *)"invalid distance too far back";
          state->mode = BAD;
          break;
        }
#endif
        hold >>= op;
        bits -= op;
        Tracevv((stderr, "inflate:         distance %u\n", dist));
        op = (unsigned)(out - beg);
        if (dist > op) {
          op = dist - op;
          if (op > whave) {
            if (state->sane) {
              strm->msg = (char *)"invalid distance too far back";
              state->mode = BAD;
              break;
            }
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
            if (len <= op - whave) {
              do {
                *out++ = 0;
              } while (--len);
              continue;
            }
            len -= op - whave;
            do {
              *out++ = 0;
            } while (--op > whave);
            if (op == 0) {
              from = out - dist;
              do {
                *out++ = *from++;
              } while (--len);
              continue;
            }
#endif
          }
          from = window;
          if (wnext >= op) {
            from += wnext - op;
          } else {
            op -= wnext;
            from += wsize - op;
            if (op < len) {
              len -= op;
              out = chunkcopy_safe(out, from, op, limit);
              from = window;
              op = wnext;
            }
          }
          if (op < len) {
            out = chunkcopy_safe(out, from, op, limit);
            len -= op;

            out = chunkunroll_relaxed(out, &dist, &len);
            out = chunkcopy_safe_ugly(out, dist, len, limit);
          } else {

            out = chunkcopy_safe(out, from, len, limit);
          }
        } else {

          out = chunkcopy_lapped_relaxed(out, dist, len);
        }
      } else if ((op & 64) == 0) {
        here = dcode + here->val + (hold & ((1U << op) - 1));
        goto dodist;
      } else {
        strm->msg = (char *)"invalid distance code";
        state->mode = BAD;
        break;
      }
    } else if ((op & 64) == 0) {
      here = lcode + here->val + (hold & ((1U << op) - 1));
      goto dolen;
    } else if (op & 32) {
      Tracevv((stderr, "inflate:         end of block\n"));
      state->mode = TYPE;
      break;
    } else {
      strm->msg = (char *)"invalid literal/length code";
      state->mode = BAD;
      break;
    }
  } while (in < last && out < end);

  len = bits >> 3;
  in -= len;
  bits -= len << 3;
  hold &= (1U << bits) - 1;

  strm->next_in = in;
  strm->next_out = out;
  strm->avail_in =
      (unsigned)(in < last ? (INFLATE_FAST_MIN_INPUT - 1) + (last - in)
                           : (INFLATE_FAST_MIN_INPUT - 1) - (in - last));
  strm->avail_out =
      (unsigned)(out < end ? (INFLATE_FAST_MIN_OUTPUT - 1) + (end - out)
                           : (INFLATE_FAST_MIN_OUTPUT - 1) - (out - end));
  state->hold = hold;
  state->bits = bits;

  Assert((state->hold >> state->bits) == 0, "invalid input data state");
}

#endif
