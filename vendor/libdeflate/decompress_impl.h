#ifndef LIB_X86_DECOMPRESS_IMPL_H
#define LIB_X86_DECOMPRESS_IMPL_H

#include "cpu_features_x86.h"
#include "decompress_defs.h"

#if defined(__GNUC__) || defined(__clang__) || MSVC_PREREQ(1930)
#define deflate_decompress_bmi2 deflate_decompress_bmi2
#define FUNCNAME deflate_decompress_bmi2
#define ATTRIBUTES _target_attribute("bmi2")

#ifndef __clang__
#ifdef ARCH_X86_64
#define EXTRACT_VARBITS(word, count) _bzhi_u64((word), (count))
#define EXTRACT_VARBITS8(word, count) _bzhi_u64((word), (count))
#else
#define EXTRACT_VARBITS(word, count) _bzhi_u32((word), (count))
#define EXTRACT_VARBITS8(word, count) _bzhi_u32((word), (count))
#endif
#endif
#include "decompress_template.h"
#endif

#if defined(deflate_decompress_bmi2) && HAVE_BMI2_NATIVE
#define DEFAULT_IMPL deflate_decompress_bmi2
#define LIBDEFLATE_HAVE_ARCH_SELECT_DECOMPRESS_FUNC
#else
static inline decompress_func_t arch_select_decompress_func(void) {
#ifdef deflate_decompress_bmi2
  if (HAVE_BMI2(get_x86_cpu_features()))
    return deflate_decompress_bmi2;
#endif
  return NULL;
}
#define LIBDEFLATE_HAVE_ARCH_SELECT_DECOMPRESS_FUNC
#endif

#endif
