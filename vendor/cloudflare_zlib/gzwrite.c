#include "gzguts.h"

local int gz_init(gz_statep);
local int gz_comp(gz_statep, int);
local int gz_zero(gz_statep, z_off64_t);

static int gz_init(gz_statep state) {
  int ret;
  z_streamp strm = &(state->strm);

  state->in = (unsigned char *)malloc(state->want << 1);
  if (state->in == NULL) {
    gz_error(state, Z_MEM_ERROR, "out of memory");
    return -1;
  }

  if (!state->direct) {

    state->out = (unsigned char *)malloc(state->want);
    if (state->out == NULL) {
      free(state->in);
      gz_error(state, Z_MEM_ERROR, "out of memory");
      return -1;
    }

    strm->zalloc = Z_NULL;
    strm->zfree = Z_NULL;
    strm->opaque = Z_NULL;
    ret = deflateInit2(strm, state->level, Z_DEFLATED, MAX_WBITS + 16,
                       DEF_MEM_LEVEL, state->strategy);
    if (ret != Z_OK) {
      free(state->out);
      free(state->in);
      gz_error(state, Z_MEM_ERROR, "out of memory");
      return -1;
    }
    strm->next_in = NULL;
  }

  state->size = state->want;

  if (!state->direct) {
    strm->avail_out = state->size;
    strm->next_out = state->out;
    state->x.next = strm->next_out;
  }
  return 0;
}

static int gz_comp(gz_statep state, int flush) {
  int ret;
  z_ssize_t got;
  unsigned have;
  z_streamp strm = &(state->strm);

  if (state->size == 0 && gz_init(state) == -1)
    return -1;

  if (state->direct) {
    while (strm->avail_in) {
      got = write(state->fd, strm->next_in, strm->avail_in);
      if (got < 0) {
        gz_error(state, Z_ERRNO, zstrerror());
        return -1;
      }
      strm->avail_in -= got;
      strm->next_in += got;
    }
    return 0;
  }

  if (state->reset) {

    if (strm->avail_in == 0)
      return 0;
    deflateReset(strm);
    state->reset = 0;
  }

  ret = Z_OK;
  do {

    if (strm->avail_out == 0 ||
        (flush != Z_NO_FLUSH && (flush != Z_FINISH || ret == Z_STREAM_END))) {
      while (strm->next_out > state->x.next) {
        got = write(state->fd, state->x.next, strm->next_out - state->x.next);
        if (got < 0) {
          gz_error(state, Z_ERRNO, zstrerror());
          return -1;
        }
        state->x.next += got;
      }
      if (strm->avail_out == 0) {
        strm->avail_out = state->size;
        strm->next_out = state->out;
        state->x.next = state->out;
      }
    }

    have = strm->avail_out;
    ret = deflate(strm, flush);
    if (ret == Z_STREAM_ERROR) {
      gz_error(state, Z_STREAM_ERROR, "internal error: deflate stream corrupt");
      return -1;
    }
    have -= strm->avail_out;
  } while (have);

  if (flush == Z_FINISH)
    state->reset = 1;

  return 0;
}

static int gz_zero(gz_statep state, z_off64_t len) {
  int first;
  unsigned n;
  z_streamp strm = &(state->strm);

  if (strm->avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
    return -1;

  first = 1;
  while (len) {
    n = GT_OFF(state->size) || (z_off64_t)state->size > len ? (unsigned)len
                                                            : state->size;
    if (first) {
      memset(state->in, 0, n);
      first = 0;
    }
    strm->avail_in = n;
    strm->next_in = state->in;
    state->x.pos += n;
    if (gz_comp(state, Z_NO_FLUSH) == -1)
      return -1;
    len -= n;
  }
  return 0;
}

int ZEXPORT gzwrite(gzFile file, voidpc buf, unsigned len) {
  unsigned put = len;
  gz_statep state;
  z_streamp strm;

  if (file == NULL)
    return 0;
  state = (gz_statep)file;
  strm = &(state->strm);

  if (state->mode != GZ_WRITE || state->err != Z_OK)
    return 0;

  if ((int)len < 0) {
    gz_error(state, Z_DATA_ERROR, "requested length does not fit in int");
    return 0;
  }

  if (len == 0)
    return 0;

  if (state->size == 0 && gz_init(state) == -1)
    return 0;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return 0;
  }

  if (len < state->size) {

    do {
      unsigned have, copy;

      if (strm->avail_in == 0)
        strm->next_in = state->in;
      have = (unsigned)((strm->next_in + strm->avail_in) - state->in);
      copy = state->size - have;
      if (copy > len)
        copy = len;
      memcpy(state->in + have, buf, copy);
      strm->avail_in += copy;
      state->x.pos += copy;
      buf = (const char *)buf + copy;
      len -= copy;
      if (len && gz_comp(state, Z_NO_FLUSH) == -1)
        return 0;
    } while (len);
  } else {

    if (strm->avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
      return 0;

    strm->avail_in = len;
    strm->next_in = (z_const Bytef *)buf;
    state->x.pos += len;
    if (gz_comp(state, Z_NO_FLUSH) == -1)
      return 0;
  }

  return (int)put;
}

int ZEXPORT gzputc(gzFile file, int c) {
  unsigned have;
  unsigned char buf[1];
  gz_statep state;
  z_streamp strm;

  if (file == NULL)
    return -1;
  state = (gz_statep)file;
  strm = &(state->strm);

  if (state->mode != GZ_WRITE || state->err != Z_OK)
    return -1;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return -1;
  }

  if (state->size) {
    if (strm->avail_in == 0)
      strm->next_in = state->in;
    have = (unsigned)((strm->next_in + strm->avail_in) - state->in);
    if (have < state->size) {
      state->in[have] = c;
      strm->avail_in++;
      state->x.pos++;
      return c & 0xff;
    }
  }

  buf[0] = c;
  if (gzwrite(file, buf, 1) != 1)
    return -1;
  return c & 0xff;
}

int ZEXPORT gzputs(gzFile file, const char *str) {
  int ret;
  unsigned len;

  len = (unsigned)strlen(str);
  ret = gzwrite(file, str, len);
  return ret == 0 && len != 0 ? -1 : ret;
}

#if defined(STDC) || defined(Z_HAVE_STDARG_H)
#include <stdarg.h>

int ZEXPORTVA gzvprintf(gzFile file, const char *format, va_list va) {
  unsigned len, left;
  char *next;
  gz_statep state;
  z_streamp strm;

  if (file == NULL)
    return -1;
  state = (gz_statep)file;
  strm = &(state->strm);

  if (state->mode != GZ_WRITE || state->err != Z_OK)
    return 0;

  if (state->size == 0 && gz_init(state) == -1)
    return 0;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return 0;
  }

  if (strm->avail_in == 0)
    strm->next_in = state->in;
  next = (char *)(strm->next_in + strm->avail_in);
  next[state->size - 1] = 0;
#ifdef NO_vsnprintf
#ifdef HAS_vsprintf_void
  (void)vsprintf(next, format, va);
  for (len = 0; len < state->size; len++)
    if (next[len] == 0)
      break;
#else
  len = vsprintf(next, format, va);
#endif
#else
#ifdef HAS_vsnprintf_void
  (void)vsnprintf(next, state->size, format, va);
  len = strlen(next);
#else
  len = vsnprintf(next, state->size, format, va);
#endif
#endif

  if (len == 0 || len >= state->size || next[state->size - 1] != 0)
    return 0;

  strm->avail_in += len;
  state->x.pos += len;
  if (strm->avail_in >= state->size) {
    left = strm->avail_in - state->size;
    strm->avail_in = state->size;
    if (gz_comp(state, Z_NO_FLUSH) == -1)
      return state->err;
    memmove(state->in, state->in + state->size, left);
    strm->next_in = state->in;
    strm->avail_in = left;
  }
  return (int)len;
}

int ZEXPORTVA gzprintf(gzFile file, const char *format, ...) {
  va_list va;
  int ret;

  va_start(va, format);
  ret = gzvprintf(file, format, va);
  va_end(va);
  return ret;
}

#else

int ZEXPORTVA gzprintf(file, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                       a11, a12, a13, a14, a15, a16, a17, a18, a19, a20)
gzFile file;
const char *format;
int a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17,
    a18, a19, a20;
{
  unsigned len, left;
  char *next;
  gz_statep state;
  z_streamp strm;

  if (file == NULL)
    return -1;
  state = (gz_statep)file;
  strm = &(state->strm);

  if (sizeof(int) != sizeof(void *))
    return 0;

  if (state->mode != GZ_WRITE || state->err != Z_OK)
    return 0;

  if (state->size == 0 && gz_init(state) == -1)
    return 0;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return 0;
  }

  if (strm->avail_in == 0)
    strm->next_in = state->in;
  next = (char *)(strm->next_in + strm->avail_in);
  next[state->size - 1] = 0;
#ifdef NO_snprintf
#ifdef HAS_sprintf_void
  sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13,
          a14, a15, a16, a17, a18, a19, a20);
  for (len = 0; len < size; len++)
    if (next[len] == 0)
      break;
#else
  len = sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12,
                a13, a14, a15, a16, a17, a18, a19, a20);
#endif
#else
#ifdef HAS_snprintf_void
  snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
           a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
  len = strlen(next);
#else
  len = snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                 a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
#endif
#endif

  if (len == 0 || len >= state->size || next[state->size - 1] != 0)
    return 0;

  strm->avail_in += len;
  state->x.pos += len;
  if (strm->avail_in >= state->size) {
    left = strm->avail_in - state->size;
    strm->avail_in = state->size;
    if (gz_comp(state, Z_NO_FLUSH) == -1)
      return state->err;
    memmove(state->in, state->in + state->size, left);
    strm->next_in = state->in;
    strm->avail_in = left;
  }
  return (int)len;
}

#endif

int ZEXPORT gzflush(gzFile file, int flush) {
  gz_statep state;

  if (file == NULL)
    return -1;
  state = (gz_statep)file;

  if (state->mode != GZ_WRITE || state->err != Z_OK)
    return Z_STREAM_ERROR;

  if (flush < 0 || flush > Z_FINISH)
    return Z_STREAM_ERROR;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return -1;
  }

  (void)gz_comp(state, flush);
  return state->err;
}

int ZEXPORT gzsetparams(gzFile file, int level, int strategy) {
  gz_statep state;
  z_streamp strm;

  if (file == NULL)
    return Z_STREAM_ERROR;
  state = (gz_statep)file;
  strm = &(state->strm);

  if (state->mode != GZ_WRITE || state->err != Z_OK || state->direct)
    return Z_STREAM_ERROR;

  if (level == state->level && strategy == state->strategy)
    return Z_OK;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      return -1;
  }

  if (state->size) {

    if (strm->avail_in && gz_comp(state, Z_BLOCK) == -1)
      return state->err;
    deflateParams(strm, level, strategy);
  }
  state->level = level;
  state->strategy = strategy;
  return Z_OK;
}

int ZEXPORT gzclose_w(gzFile file) {
  int ret = Z_OK;
  gz_statep state;

  if (file == NULL)
    return Z_STREAM_ERROR;
  state = (gz_statep)file;

  if (state->mode != GZ_WRITE)
    return Z_STREAM_ERROR;

  if (state->seek) {
    state->seek = 0;
    if (gz_zero(state, state->skip) == -1)
      ret = state->err;
  }

  if (gz_comp(state, Z_FINISH) == -1)
    ret = state->err;
  if (state->size) {
    if (!state->direct) {
      (void)deflateEnd(&(state->strm));
      free(state->out);
    }
    free(state->in);
  }
  gz_error(state, Z_OK, NULL);
  free(state->path);
  if (close(state->fd) == -1)
    ret = Z_ERRNO;
  free(state);
  return ret;
}
