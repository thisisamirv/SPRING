

#ifndef LIBDEFLATE_H
#define LIBDEFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBDEFLATE_VERSION_MAJOR 1
#define LIBDEFLATE_VERSION_MINOR 25
#define LIBDEFLATE_VERSION_STRING "1.25"

#ifndef LIBDEFLATEAPI
#if defined(LIBDEFLATE_DLL) && (defined(_WIN32) || defined(__CYGWIN__))
#define LIBDEFLATEAPI __declspec(dllimport)
#else
#define LIBDEFLATEAPI
#endif
#endif

struct libdeflate_compressor;
struct libdeflate_options;

LIBDEFLATEAPI struct libdeflate_compressor *
libdeflate_alloc_compressor(int compression_level);

LIBDEFLATEAPI struct libdeflate_compressor *
libdeflate_alloc_compressor_ex(int compression_level,
                               const struct libdeflate_options *options);

LIBDEFLATEAPI size_t libdeflate_deflate_compress(
    struct libdeflate_compressor *compressor, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail);

LIBDEFLATEAPI size_t libdeflate_deflate_compress_bound(
    struct libdeflate_compressor *compressor, size_t in_nbytes);

LIBDEFLATEAPI size_t libdeflate_zlib_compress(
    struct libdeflate_compressor *compressor, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail);

LIBDEFLATEAPI size_t libdeflate_zlib_compress_bound(
    struct libdeflate_compressor *compressor, size_t in_nbytes);

LIBDEFLATEAPI size_t libdeflate_gzip_compress(
    struct libdeflate_compressor *compressor, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail);

LIBDEFLATEAPI size_t libdeflate_gzip_compress_bound(
    struct libdeflate_compressor *compressor, size_t in_nbytes);

LIBDEFLATEAPI void
libdeflate_free_compressor(struct libdeflate_compressor *compressor);

struct libdeflate_decompressor;
struct libdeflate_options;

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void);

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor_ex(const struct libdeflate_options *options);

enum libdeflate_result {

  LIBDEFLATE_SUCCESS = 0,

  LIBDEFLATE_BAD_DATA = 1,

  LIBDEFLATE_SHORT_OUTPUT = 2,

  LIBDEFLATE_INSUFFICIENT_SPACE = 3,
};

LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor *decompressor,
                              const void *in, size_t in_nbytes, void *out,
                              size_t out_nbytes_avail,
                              size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI enum libdeflate_result libdeflate_deflate_decompress_ex(
    struct libdeflate_decompressor *decompressor, const void *in,
    size_t in_nbytes, void *out, size_t out_nbytes_avail,
    size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI enum libdeflate_result
libdeflate_zlib_decompress(struct libdeflate_decompressor *decompressor,
                           const void *in, size_t in_nbytes, void *out,
                           size_t out_nbytes_avail,
                           size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI enum libdeflate_result libdeflate_zlib_decompress_ex(
    struct libdeflate_decompressor *decompressor, const void *in,
    size_t in_nbytes, void *out, size_t out_nbytes_avail,
    size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor *decompressor,
                           const void *in, size_t in_nbytes, void *out,
                           size_t out_nbytes_avail,
                           size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI enum libdeflate_result libdeflate_gzip_decompress_ex(
    struct libdeflate_decompressor *decompressor, const void *in,
    size_t in_nbytes, void *out, size_t out_nbytes_avail,
    size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

LIBDEFLATEAPI void
libdeflate_free_decompressor(struct libdeflate_decompressor *decompressor);

LIBDEFLATEAPI uint32_t libdeflate_adler32(uint32_t adler, const void *buffer,
                                          size_t len);

LIBDEFLATEAPI uint32_t libdeflate_crc32(uint32_t crc, const void *buffer,
                                        size_t len);

LIBDEFLATEAPI void libdeflate_set_memory_allocator(void *(*malloc_func)(size_t),
                                                   void (*free_func)(void *));

struct libdeflate_options {

  size_t sizeof_options;

  void *(*malloc_func)(size_t);
  void (*free_func)(void *);
};

#ifdef __cplusplus
}
#endif

#endif
