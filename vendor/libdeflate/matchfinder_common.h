

#ifndef LIB_MATCHFINDER_COMMON_H
#define LIB_MATCHFINDER_COMMON_H

#include "matchfinder_defs.h"

#undef matchfinder_init
#undef matchfinder_rebase
#ifdef _aligned_attribute
#define MATCHFINDER_ALIGNED _aligned_attribute(MATCHFINDER_MEM_ALIGNMENT)
#if defined(ARCH_ARM32) || defined(ARCH_ARM64)
#include "matchfinder_impl_arm.h"
#elif defined(ARCH_RISCV)
#include "matchfinder_impl_riscv.h"
#elif defined(ARCH_X86_32) || defined(ARCH_X86_64)
#include "matchfinder_impl_x86.h"
#endif
#else
#define MATCHFINDER_ALIGNED
#endif

#ifndef matchfinder_init
static forceinline void matchfinder_init(mf_pos_t *data, size_t size) {
  size_t num_entries = size / sizeof(*data);
  size_t i;

  for (i = 0; i < num_entries; i++)
    data[i] = MATCHFINDER_INITVAL;
}
#endif

#ifndef matchfinder_rebase
static forceinline void matchfinder_rebase(mf_pos_t *data, size_t size) {
  size_t num_entries = size / sizeof(*data);
  size_t i;

  if (MATCHFINDER_WINDOW_SIZE == 32768) {

    for (i = 0; i < num_entries; i++)
      data[i] = 0x8000 | (data[i] & ~(data[i] >> 15));
  } else {
    for (i = 0; i < num_entries; i++) {
      if (data[i] >= 0)
        data[i] -= (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
      else
        data[i] = (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
    }
  }
}
#endif

static forceinline u32 lz_hash(u32 seq, unsigned num_bits) {
  return (u32)(seq * 0x1E35A7BD) >> (32 - num_bits);
}

static forceinline u32 lz_extend(const u8 *const strptr,
                                 const u8 *const matchptr, const u32 start_len,
                                 const u32 max_len) {
  u32 len = start_len;
  machine_word_t v_word;

  if (UNALIGNED_ACCESS_IS_FAST) {

    if (likely(max_len - len >= 4 * WORDBYTES)) {

#define COMPARE_WORD_STEP                                                      \
  v_word =                                                                     \
      load_word_unaligned(&matchptr[len]) ^ load_word_unaligned(&strptr[len]); \
  if (v_word != 0)                                                             \
    goto word_differs;                                                         \
  len += WORDBYTES;

      COMPARE_WORD_STEP
      COMPARE_WORD_STEP
      COMPARE_WORD_STEP
      COMPARE_WORD_STEP
#undef COMPARE_WORD_STEP
    }

    while (len + WORDBYTES <= max_len) {
      v_word = load_word_unaligned(&matchptr[len]) ^
               load_word_unaligned(&strptr[len]);
      if (v_word != 0)
        goto word_differs;
      len += WORDBYTES;
    }
  }

  while (len < max_len && matchptr[len] == strptr[len])
    len++;
  return len;

word_differs:
  if (CPU_IS_LITTLE_ENDIAN())
    len += (bsfw(v_word) >> 3);
  else
    len += (WORDBITS - 1 - bsrw(v_word)) >> 3;
  return len;
}

#endif
