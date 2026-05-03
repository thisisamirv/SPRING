/* ******************************************************************
 * huff0 huffman codec,
 * part of Finite State Entropy library
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * You can contact the author at :
 * - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 ****************************************************************** */

#ifndef HUF_H_298734234
#define HUF_H_298734234

#include "mem.h"
#include "zstd_deps.h"
#define FSE_STATIC_LINKING_ONLY
#include "fse.h"

#define HUF_BLOCKSIZE_MAX (128 * 1024)

size_t HUF_compressBound(size_t size);

unsigned HUF_isError(size_t code);
const char *HUF_getErrorName(size_t code);

#define HUF_WORKSPACE_SIZE ((8 << 10) + 512)
#define HUF_WORKSPACE_SIZE_U64 (HUF_WORKSPACE_SIZE / sizeof(U64))

#define HUF_TABLELOG_MAX 12

#define HUF_TABLELOG_DEFAULT 11

#define HUF_SYMBOLVALUE_MAX 255

#define HUF_TABLELOG_ABSOLUTEMAX 12

#if (HUF_TABLELOG_MAX > HUF_TABLELOG_ABSOLUTEMAX)
#error "HUF_TABLELOG_MAX is too large !"
#endif

#define HUF_CTABLEBOUND 129
#define HUF_BLOCKBOUND(size) (size + (size >> 8) + 8)
#define HUF_COMPRESSBOUND(size) (HUF_CTABLEBOUND + HUF_BLOCKBOUND(size))

typedef size_t HUF_CElt;
#define HUF_CTABLE_SIZE_ST(maxSymbolValue) ((maxSymbolValue) + 2)
#define HUF_CTABLE_SIZE(maxSymbolValue)                                        \
  (HUF_CTABLE_SIZE_ST(maxSymbolValue) * sizeof(size_t))
#define HUF_CREATE_STATIC_CTABLE(name, maxSymbolValue)                         \
  HUF_CElt name[HUF_CTABLE_SIZE_ST(maxSymbolValue)]

typedef U32 HUF_DTable;
#define HUF_DTABLE_SIZE(maxTableLog) (1 + (1 << (maxTableLog)))
#define HUF_CREATE_STATIC_DTABLEX1(DTable, maxTableLog)                        \
  HUF_DTable DTable[HUF_DTABLE_SIZE((maxTableLog) - 1)] = {                    \
      ((U32)((maxTableLog) - 1) * 0x01000001)}
#define HUF_CREATE_STATIC_DTABLEX2(DTable, maxTableLog)                        \
  HUF_DTable DTable[HUF_DTABLE_SIZE(maxTableLog)] = {                          \
      ((U32)(maxTableLog) * 0x01000001)}

typedef enum {

  HUF_flags_bmi2 = (1 << 0),

  HUF_flags_optimalDepth = (1 << 1),

  HUF_flags_preferRepeat = (1 << 2),

  HUF_flags_suspectUncompressible = (1 << 3),

  HUF_flags_disableAsm = (1 << 4),

  HUF_flags_disableFast = (1 << 5)
} HUF_flags_e;

#define HUF_OPTIMAL_DEPTH_THRESHOLD ZSTD_btultra

unsigned HUF_minTableLog(unsigned symbolCardinality);
unsigned HUF_cardinality(const unsigned *count, unsigned maxSymbolValue);
unsigned HUF_optimalTableLog(unsigned maxTableLog, size_t srcSize,
                             unsigned maxSymbolValue, void *workSpace,
                             size_t wkspSize, HUF_CElt *table,
                             const unsigned *count, int flags);

size_t HUF_writeCTable_wksp(void *dst, size_t maxDstSize,
                            const HUF_CElt *CTable, unsigned maxSymbolValue,
                            unsigned huffLog, void *workspace,
                            size_t workspaceSize);
size_t HUF_compress4X_usingCTable(void *dst, size_t dstSize, const void *src,
                                  size_t srcSize, const HUF_CElt *CTable,
                                  int flags);
size_t HUF_estimateCompressedSize(const HUF_CElt *CTable, const unsigned *count,
                                  unsigned maxSymbolValue);
int HUF_validateCTable(const HUF_CElt *CTable, const unsigned *count,
                       unsigned maxSymbolValue);

typedef enum {
  HUF_repeat_none,
  HUF_repeat_check,

  HUF_repeat_valid

} HUF_repeat;

size_t HUF_compress4X_repeat(void *dst, size_t dstSize, const void *src,
                             size_t srcSize, unsigned maxSymbolValue,
                             unsigned tableLog, void *workSpace,
                             size_t wkspSize,

                             HUF_CElt *hufTable, HUF_repeat *repeat, int flags);

#define HUF_CTABLE_WORKSPACE_SIZE_U32 ((4 * (HUF_SYMBOLVALUE_MAX + 1)) + 192)
#define HUF_CTABLE_WORKSPACE_SIZE                                              \
  (HUF_CTABLE_WORKSPACE_SIZE_U32 * sizeof(unsigned))
size_t HUF_buildCTable_wksp(HUF_CElt *tree, const unsigned *count,
                            U32 maxSymbolValue, U32 maxNbBits, void *workSpace,
                            size_t wkspSize);

size_t HUF_readStats(BYTE *huffWeight, size_t hwSize, U32 *rankStats,
                     U32 *nbSymbolsPtr, U32 *tableLogPtr, const void *src,
                     size_t srcSize);

#define HUF_READ_STATS_WORKSPACE_SIZE_U32                                      \
  FSE_DECOMPRESS_WKSP_SIZE_U32(6, HUF_TABLELOG_MAX - 1)
#define HUF_READ_STATS_WORKSPACE_SIZE                                          \
  (HUF_READ_STATS_WORKSPACE_SIZE_U32 * sizeof(unsigned))
size_t HUF_readStats_wksp(BYTE *huffWeight, size_t hwSize, U32 *rankStats,
                          U32 *nbSymbolsPtr, U32 *tableLogPtr, const void *src,
                          size_t srcSize, void *workspace, size_t wkspSize,
                          int flags);

size_t HUF_readCTable(HUF_CElt *CTable, unsigned *maxSymbolValuePtr,
                      const void *src, size_t srcSize,
                      unsigned *hasZeroWeights);

U32 HUF_getNbBitsFromCTable(const HUF_CElt *symbolTable, U32 symbolValue);

typedef struct {
  BYTE tableLog;
  BYTE maxSymbolValue;
  BYTE unused[sizeof(size_t) - 2];
} HUF_CTableHeader;

HUF_CTableHeader HUF_readCTableHeader(HUF_CElt const *ctable);

U32 HUF_selectDecoder(size_t dstSize, size_t cSrcSize);

#define HUF_DECOMPRESS_WORKSPACE_SIZE ((2 << 10) + (1 << 9))
#define HUF_DECOMPRESS_WORKSPACE_SIZE_U32                                      \
  (HUF_DECOMPRESS_WORKSPACE_SIZE / sizeof(U32))

size_t HUF_compress1X_usingCTable(void *dst, size_t dstSize, const void *src,
                                  size_t srcSize, const HUF_CElt *CTable,
                                  int flags);

size_t HUF_compress1X_repeat(void *dst, size_t dstSize, const void *src,
                             size_t srcSize, unsigned maxSymbolValue,
                             unsigned tableLog, void *workSpace,
                             size_t wkspSize,

                             HUF_CElt *hufTable, HUF_repeat *repeat, int flags);

size_t HUF_decompress1X_DCtx_wksp(HUF_DTable *dctx, void *dst, size_t dstSize,
                                  const void *cSrc, size_t cSrcSize,
                                  void *workSpace, size_t wkspSize, int flags);
#ifndef HUF_FORCE_DECOMPRESS_X1
size_t HUF_decompress1X2_DCtx_wksp(HUF_DTable *dctx, void *dst, size_t dstSize,
                                   const void *cSrc, size_t cSrcSize,
                                   void *workSpace, size_t wkspSize, int flags);
#endif

size_t HUF_decompress1X_usingDTable(void *dst, size_t maxDstSize,
                                    const void *cSrc, size_t cSrcSize,
                                    const HUF_DTable *DTable, int flags);
#ifndef HUF_FORCE_DECOMPRESS_X2
size_t HUF_decompress1X1_DCtx_wksp(HUF_DTable *dctx, void *dst, size_t dstSize,
                                   const void *cSrc, size_t cSrcSize,
                                   void *workSpace, size_t wkspSize, int flags);
#endif
size_t HUF_decompress4X_usingDTable(void *dst, size_t maxDstSize,
                                    const void *cSrc, size_t cSrcSize,
                                    const HUF_DTable *DTable, int flags);
size_t HUF_decompress4X_hufOnly_wksp(HUF_DTable *dctx, void *dst,
                                     size_t dstSize, const void *cSrc,
                                     size_t cSrcSize, void *workSpace,
                                     size_t wkspSize, int flags);
#ifndef HUF_FORCE_DECOMPRESS_X2
size_t HUF_readDTableX1_wksp(HUF_DTable *DTable, const void *src,
                             size_t srcSize, void *workSpace, size_t wkspSize,
                             int flags);
#endif
#ifndef HUF_FORCE_DECOMPRESS_X1
size_t HUF_readDTableX2_wksp(HUF_DTable *DTable, const void *src,
                             size_t srcSize, void *workSpace, size_t wkspSize,
                             int flags);
#endif

#endif
