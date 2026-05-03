

#ifndef LIB_LIB_COMMON_H
#define LIB_LIB_COMMON_H

#ifdef LIBDEFLATE_H

#error "lib_common.h must always be included before libdeflate.h"
#endif

#if defined(LIBDEFLATE_DLL) && (defined(_WIN32) || defined(__CYGWIN__))
#define LIBDEFLATE_EXPORT_SYM __declspec(dllexport)
#elif defined(__GNUC__)
#define LIBDEFLATE_EXPORT_SYM __attribute__((visibility("default")))
#else
#define LIBDEFLATE_EXPORT_SYM
#endif

#if defined(__GNUC__) && defined(__i386__)
#define LIBDEFLATE_ALIGN_STACK __attribute__((force_align_arg_pointer))
#else
#define LIBDEFLATE_ALIGN_STACK
#endif

#define LIBDEFLATEAPI LIBDEFLATE_EXPORT_SYM LIBDEFLATE_ALIGN_STACK

#include "common_defs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef u8 libdeflate_byte_t;

typedef void *(*malloc_func_t)(size_t);
typedef void (*free_func_t)(void *);

extern malloc_func_t libdeflate_default_malloc_func;
extern free_func_t libdeflate_default_free_func;

void *libdeflate_aligned_malloc(malloc_func_t malloc_func, size_t alignment,
                                size_t size);
void libdeflate_aligned_free(free_func_t free_func, void *ptr);

#ifdef FREESTANDING

void *memset(void *s, int c, size_t n);
#define memset(s, c, n) __builtin_memset((s), (c), (n))

void *memcpy(void *dest, const void *src, size_t n);
#define memcpy(dest, src, n) __builtin_memcpy((dest), (src), (n))

void *memmove(void *dest, const void *src, size_t n);
#define memmove(dest, src, n) __builtin_memmove((dest), (src), (n))

int memcmp(const void *s1, const void *s2, size_t n);
#define memcmp(s1, s2, n) __builtin_memcmp((s1), (s2), (n))

#undef LIBDEFLATE_ENABLE_ASSERTIONS
#else
#include <string.h>

#ifdef __clang_analyzer__
#define LIBDEFLATE_ENABLE_ASSERTIONS
#endif
#endif

#ifdef LIBDEFLATE_ENABLE_ASSERTIONS
NORETURN void libdeflate_assertion_failed(const char *expr, const char *file,
                                          int line);
#define ASSERT(expr)                                                           \
  {                                                                            \
    if (unlikely(!(expr)))                                                     \
      libdeflate_assertion_failed(#expr, __FILE__, __LINE__);                  \
  }
#else
#define ASSERT(expr) (void)(expr)
#endif

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)
#define ADD_SUFFIX(name) CONCAT(name, SUFFIX)

#endif
