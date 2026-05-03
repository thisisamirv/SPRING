#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include "common_defs.h"

#include <immintrin.h>

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)
#define ADD_SUFFIX(name) CONCAT(name, SUFFIX)
#include "crc32_multipliers.h"
#ifndef SUFFIX
#define SUFFIX _pclmul_dummy
#define ATTRIBUTES _target_attribute("pclmul,sse4.1")

#define VL 16
#define USE_AVX512 0

extern const u8 shift_tab[48];
#ifndef crc32_slice1
static u32 crc32_slice1(u32 crc, const u8 *p, size_t len) {

  (void)crc;
  (void)p;
  (void)len;
  return 0;
}
#endif
#endif

#if VL == 16
#define vec_t __m128i
#define fold_vec fold_vec128
#define VLOADU(p) _mm_loadu_si128((const __m128i_u *)(p))
#define VXOR(a, b) _mm_xor_si128((a), (b))
#define M128I_TO_VEC(a) a
#define MULTS_8V _mm_set_epi64x(CRC32_X991_MODG, CRC32_X1055_MODG)
#define MULTS_4V _mm_set_epi64x(CRC32_X479_MODG, CRC32_X543_MODG)
#define MULTS_2V _mm_set_epi64x(CRC32_X223_MODG, CRC32_X287_MODG)
#define MULTS_1V _mm_set_epi64x(CRC32_X95_MODG, CRC32_X159_MODG)
#elif VL == 32
#define vec_t __m256i
#define fold_vec fold_vec256
#define VLOADU(p) _mm256_loadu_si256((const __m256i_u *)(p))
#define VXOR(a, b) _mm256_xor_si256((a), (b))
#define M128I_TO_VEC(a) _mm256_zextsi128_si256(a)
#define MULTS(a, b) _mm256_set_epi64x(a, b, a, b)
#define MULTS_8V MULTS(CRC32_X2015_MODG, CRC32_X2079_MODG)
#define MULTS_4V MULTS(CRC32_X991_MODG, CRC32_X1055_MODG)
#define MULTS_2V MULTS(CRC32_X479_MODG, CRC32_X543_MODG)
#define MULTS_1V MULTS(CRC32_X223_MODG, CRC32_X287_MODG)
#elif VL == 64
#define vec_t __m512i
#define fold_vec fold_vec512
#define VLOADU(p) _mm512_loadu_si512((const __m512i_u *)(p))
#define VXOR(a, b) _mm512_xor_si512((a), (b))
#define M128I_TO_VEC(a) _mm512_zextsi128_si512(a)
#define MULTS(a, b) _mm512_set_epi64(a, b, a, b, a, b, a, b)
#define MULTS_8V MULTS(CRC32_X4063_MODG, CRC32_X4127_MODG)
#define MULTS_4V MULTS(CRC32_X2015_MODG, CRC32_X2079_MODG)
#define MULTS_2V MULTS(CRC32_X991_MODG, CRC32_X1055_MODG)
#define MULTS_1V MULTS(CRC32_X479_MODG, CRC32_X543_MODG)
#else
#error "unsupported vector length"
#endif

#undef fold_vec128
static inline ATTRIBUTES
    __m128i ADD_SUFFIX(fold_vec128)(__m128i src, __m128i dst, __m128i mults) {

  dst = _mm_xor_si128(dst, _mm_clmulepi64_si128(src, mults, 0x00));
  dst = _mm_xor_si128(dst, _mm_clmulepi64_si128(src, mults, 0x11));
  return dst;
}
#define fold_vec128 ADD_SUFFIX(fold_vec128)

#if VL >= 32
#undef fold_vec256
static inline ATTRIBUTES
    __m256i ADD_SUFFIX(fold_vec256)(__m256i src, __m256i dst, __m256i mults) {

#if USE_AVX512

  return _mm256_ternarylogic_epi32(_mm256_clmulepi64_epi128(src, mults, 0x00),
                                   _mm256_clmulepi64_epi128(src, mults, 0x11),
                                   dst, 0x96);
#else
  return _mm256_xor_si256(
      _mm256_xor_si256(dst, _mm256_clmulepi64_epi128(src, mults, 0x00)),
      _mm256_clmulepi64_epi128(src, mults, 0x11));
#endif
}
#define fold_vec256 ADD_SUFFIX(fold_vec256)
#endif

#if VL >= 64
#undef fold_vec512
static inline ATTRIBUTES
    __m512i ADD_SUFFIX(fold_vec512)(__m512i src, __m512i dst, __m512i mults) {

  return _mm512_ternarylogic_epi32(_mm512_clmulepi64_epi128(src, mults, 0x00),
                                   _mm512_clmulepi64_epi128(src, mults, 0x11),
                                   dst, 0x96);
}
#define fold_vec512 ADD_SUFFIX(fold_vec512)
#endif

#undef fold_lessthan16bytes
static inline ATTRIBUTES
    __m128i ADD_SUFFIX(fold_lessthan16bytes)(__m128i x, const u8 *p, size_t len,
                                             __m128i mults_128b) {

  __m128i lshift = _mm_loadu_si128((const __m128i_u *)&shift_tab[len]);
  __m128i rshift = _mm_loadu_si128((const __m128i_u *)&shift_tab[len + 16]);
  __m128i x0, x1;

  x0 = _mm_shuffle_epi8(x, lshift);

  x1 = _mm_blendv_epi8(_mm_shuffle_epi8(x, rshift),
                       _mm_loadu_si128((const __m128i_u *)(p + len - 16)),

                       rshift);

  return fold_vec128(x0, x1, mults_128b);
}
#define fold_lessthan16bytes ADD_SUFFIX(fold_lessthan16bytes)

static ATTRIBUTES MAYBE_UNUSED u32 ADD_SUFFIX(crc32_x86)(u32 crc, const u8 *p,
                                                         size_t len) {

  const vec_t mults_8v = MULTS_8V;
  const vec_t mults_4v = MULTS_4V;
  const vec_t mults_2v = MULTS_2V;
  const vec_t mults_1v = MULTS_1V;
  const __m128i mults_128b = _mm_set_epi64x(CRC32_X95_MODG, CRC32_X159_MODG);
  const __m128i barrett_reduction_constants =
      _mm_set_epi64x(CRC32_BARRETT_CONSTANT_2, CRC32_BARRETT_CONSTANT_1);
  vec_t v0, v1, v2, v3, v4, v5, v6, v7;
  __m128i x0 = _mm_cvtsi32_si128(crc);
  __m128i x1;

  if (len < 8 * VL) {
    if (len < VL) {
      STATIC_ASSERT(VL == 16 || VL == 32 || VL == 64);
      if (len < 16) {
#if USE_AVX512
        if (len < 4)
          return crc32_slice1(crc, p, len);

        x0 = _mm_xor_si128(x0, _mm_maskz_loadu_epi8((1 << len) - 1, p));
        x0 = _mm_shuffle_epi8(
            x0, _mm_loadu_si128((const __m128i_u *)&shift_tab[len]));
        goto reduce_x0;
#else
        return crc32_slice1(crc, p, len);
#endif
      }

      x0 = _mm_xor_si128(_mm_loadu_si128((const __m128i_u *)p), x0);
      if (len >= 32) {
        x0 = fold_vec128(x0, _mm_loadu_si128((const __m128i_u *)(p + 16)),
                         mults_128b);
        if (len >= 48)
          x0 = fold_vec128(x0, _mm_loadu_si128((const __m128i_u *)(p + 32)),
                           mults_128b);
      }
      p += len & ~15;
      goto less_than_16_remaining;
    }
    v0 = VXOR(VLOADU(p), M128I_TO_VEC(x0));
    if (len < 2 * VL) {
      p += VL;
      goto less_than_vl_remaining;
    }
    v1 = VLOADU(p + 1 * VL);
    if (len < 4 * VL) {
      p += 2 * VL;
      goto less_than_2vl_remaining;
    }
    v2 = VLOADU(p + 2 * VL);
    v3 = VLOADU(p + 3 * VL);
    p += 4 * VL;
  } else {

    if (len > 65536 && ((uintptr_t)p & (VL - 1))) {
      size_t align = -(uintptr_t)p & (VL - 1);

      len -= align;
      x0 = _mm_xor_si128(_mm_loadu_si128((const __m128i_u *)p), x0);
      p += 16;
      if (align & 15) {
        x0 = fold_lessthan16bytes(x0, p, align & 15, mults_128b);
        p += align & 15;
        align &= ~15;
      }
      while (align) {
        x0 = fold_vec128(x0, *(const __m128i *)p, mults_128b);
        p += 16;
        align -= 16;
      }
      v0 = M128I_TO_VEC(x0);
#if VL == 32
      v0 = _mm256_inserti128_si256(v0, *(const __m128i *)p, 1);
#elif VL == 64
      v0 = _mm512_inserti32x4(v0, *(const __m128i *)p, 1);
      v0 = _mm512_inserti64x4(v0, *(const __m256i *)(p + 16), 1);
#endif
      p -= 16;
    } else {
      v0 = VXOR(VLOADU(p), M128I_TO_VEC(x0));
    }
    v1 = VLOADU(p + 1 * VL);
    v2 = VLOADU(p + 2 * VL);
    v3 = VLOADU(p + 3 * VL);
    v4 = VLOADU(p + 4 * VL);
    v5 = VLOADU(p + 5 * VL);
    v6 = VLOADU(p + 6 * VL);
    v7 = VLOADU(p + 7 * VL);
    p += 8 * VL;

    while (len >= 16 * VL) {
      v0 = fold_vec(v0, VLOADU(p + 0 * VL), mults_8v);
      v1 = fold_vec(v1, VLOADU(p + 1 * VL), mults_8v);
      v2 = fold_vec(v2, VLOADU(p + 2 * VL), mults_8v);
      v3 = fold_vec(v3, VLOADU(p + 3 * VL), mults_8v);
      v4 = fold_vec(v4, VLOADU(p + 4 * VL), mults_8v);
      v5 = fold_vec(v5, VLOADU(p + 5 * VL), mults_8v);
      v6 = fold_vec(v6, VLOADU(p + 6 * VL), mults_8v);
      v7 = fold_vec(v7, VLOADU(p + 7 * VL), mults_8v);
      p += 8 * VL;
      len -= 8 * VL;
    }

    v0 = fold_vec(v0, v4, mults_4v);
    v1 = fold_vec(v1, v5, mults_4v);
    v2 = fold_vec(v2, v6, mults_4v);
    v3 = fold_vec(v3, v7, mults_4v);
    if (len & (4 * VL)) {
      v0 = fold_vec(v0, VLOADU(p + 0 * VL), mults_4v);
      v1 = fold_vec(v1, VLOADU(p + 1 * VL), mults_4v);
      v2 = fold_vec(v2, VLOADU(p + 2 * VL), mults_4v);
      v3 = fold_vec(v3, VLOADU(p + 3 * VL), mults_4v);
      p += 4 * VL;
    }
  }

  v0 = fold_vec(v0, v2, mults_2v);
  v1 = fold_vec(v1, v3, mults_2v);
  if (len & (2 * VL)) {
    v0 = fold_vec(v0, VLOADU(p + 0 * VL), mults_2v);
    v1 = fold_vec(v1, VLOADU(p + 1 * VL), mults_2v);
    p += 2 * VL;
  }
less_than_2vl_remaining:

  v0 = fold_vec(v0, v1, mults_1v);
  if (len & VL) {
    v0 = fold_vec(v0, VLOADU(p), mults_1v);
    p += VL;
  }
less_than_vl_remaining:

#if VL == 16
  x0 = v0;
#else
{
#if VL == 32
  __m256i y0 = v0;
#else
  const __m256i mults_256b = _mm256_set_epi64x(
      CRC32_X223_MODG, CRC32_X287_MODG, CRC32_X223_MODG, CRC32_X287_MODG);
  __m256i y0 = fold_vec256(_mm512_extracti64x4_epi64(v0, 0),
                           _mm512_extracti64x4_epi64(v0, 1), mults_256b);
  if (len & 32) {
    y0 = fold_vec256(y0, _mm256_loadu_si256((const __m256i_u *)p), mults_256b);
    p += 32;
  }
#endif
  x0 = fold_vec128(_mm256_extracti128_si256(y0, 0),
                   _mm256_extracti128_si256(y0, 1), mults_128b);
}
  if (len & 16) {
    x0 = fold_vec128(x0, _mm_loadu_si128((const __m128i_u *)p), mults_128b);
    p += 16;
  }
#endif
less_than_16_remaining:
  len &= 15;

  if (len)
    x0 = fold_lessthan16bytes(x0, p, len, mults_128b);
#if USE_AVX512
reduce_x0:
#endif

  x0 = _mm_xor_si128(_mm_clmulepi64_si128(x0, mults_128b, 0x10),
                     _mm_bsrli_si128(x0, 8));
  x1 = _mm_clmulepi64_si128(x0, barrett_reduction_constants, 0x00);
  x1 = _mm_clmulepi64_si128(x1, barrett_reduction_constants, 0x10);
  x0 = _mm_xor_si128(x0, x1);
  return _mm_extract_epi32(x0, 2);
}

#undef vec_t
#undef fold_vec
#undef VLOADU
#undef VXOR
#undef M128I_TO_VEC
#undef MULTS
#undef MULTS_8V
#undef MULTS_4V
#undef MULTS_2V
#undef MULTS_1V
#undef fold_vec128
#undef fold_vec256
#undef fold_vec512
#undef fold_lessthan16bytes

#undef SUFFIX
#undef ATTRIBUTES
#undef VL
#undef USE_AVX512
#undef CONCAT_IMPL
#undef CONCAT
#undef ADD_SUFFIX
#endif
