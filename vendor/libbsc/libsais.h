/*--

This file is a part of libsais, a library for linear time suffix array,
longest common prefix array and burrows wheeler transform construction.

   Copyright (c) 2021-2025 Ilya Grebnov <ilya.grebnov@gmail.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

Please see the file LICENSE for full copyright information.

--*/

#ifndef LIBSAIS_H
#define LIBSAIS_H 1

#define LIBSAIS_VERSION_MAJOR 2
#define LIBSAIS_VERSION_MINOR 10
#define LIBSAIS_VERSION_PATCH 4
#define LIBSAIS_VERSION_STRING "2.10.4"

#ifdef _WIN32
#ifdef LIBSAIS_SHARED
#ifdef LIBSAIS_EXPORTS
#define LIBSAIS_API __declspec(dllexport)
#else
#define LIBSAIS_API __declspec(dllimport)
#endif
#else
#define LIBSAIS_API
#endif
#else
#define LIBSAIS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

LIBSAIS_API void *libsais_create_ctx(void);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API void *libsais_create_ctx_omp(int32_t threads);
#endif

LIBSAIS_API void libsais_free_ctx(void *ctx);

LIBSAIS_API int32_t libsais(const uint8_t *T, int32_t *SA, int32_t n,
                            int32_t fs, int32_t *freq);

LIBSAIS_API int32_t libsais_gsa(const uint8_t *T, int32_t *SA, int32_t n,
                                int32_t fs, int32_t *freq);

LIBSAIS_API int32_t libsais_int(int32_t *T, int32_t *SA, int32_t n, int32_t k,
                                int32_t fs);

LIBSAIS_API int32_t libsais_ctx(const void *ctx, const uint8_t *T, int32_t *SA,
                                int32_t n, int32_t fs, int32_t *freq);

LIBSAIS_API int32_t libsais_gsa_ctx(const void *ctx, const uint8_t *T,
                                    int32_t *SA, int32_t n, int32_t fs,
                                    int32_t *freq);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API int32_t libsais_omp(const uint8_t *T, int32_t *SA, int32_t n,
                                int32_t fs, int32_t *freq, int32_t threads);

LIBSAIS_API int32_t libsais_gsa_omp(const uint8_t *T, int32_t *SA, int32_t n,
                                    int32_t fs, int32_t *freq, int32_t threads);

LIBSAIS_API int32_t libsais_int_omp(int32_t *T, int32_t *SA, int32_t n,
                                    int32_t k, int32_t fs, int32_t threads);
#endif

LIBSAIS_API int32_t libsais_bwt(const uint8_t *T, uint8_t *U, int32_t *A,
                                int32_t n, int32_t fs, int32_t *freq);

LIBSAIS_API int32_t libsais_bwt_aux(const uint8_t *T, uint8_t *U, int32_t *A,
                                    int32_t n, int32_t fs, int32_t *freq,
                                    int32_t r, int32_t *I);

LIBSAIS_API int32_t libsais_bwt_ctx(const void *ctx, const uint8_t *T,
                                    uint8_t *U, int32_t *A, int32_t n,
                                    int32_t fs, int32_t *freq);

LIBSAIS_API int32_t libsais_bwt_aux_ctx(const void *ctx, const uint8_t *T,
                                        uint8_t *U, int32_t *A, int32_t n,
                                        int32_t fs, int32_t *freq, int32_t r,
                                        int32_t *I);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API int32_t libsais_bwt_omp(const uint8_t *T, uint8_t *U, int32_t *A,
                                    int32_t n, int32_t fs, int32_t *freq,
                                    int32_t threads);

LIBSAIS_API int32_t libsais_bwt_aux_omp(const uint8_t *T, uint8_t *U,
                                        int32_t *A, int32_t n, int32_t fs,
                                        int32_t *freq, int32_t r, int32_t *I,
                                        int32_t threads);
#endif

LIBSAIS_API void *libsais_unbwt_create_ctx(void);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API void *libsais_unbwt_create_ctx_omp(int32_t threads);
#endif

LIBSAIS_API void libsais_unbwt_free_ctx(void *ctx);

LIBSAIS_API int32_t libsais_unbwt(const uint8_t *T, uint8_t *U, int32_t *A,
                                  int32_t n, const int32_t *freq, int32_t i);

LIBSAIS_API int32_t libsais_unbwt_ctx(const void *ctx, const uint8_t *T,
                                      uint8_t *U, int32_t *A, int32_t n,
                                      const int32_t *freq, int32_t i);

LIBSAIS_API int32_t libsais_unbwt_aux(const uint8_t *T, uint8_t *U, int32_t *A,
                                      int32_t n, const int32_t *freq, int32_t r,
                                      const int32_t *I);

LIBSAIS_API int32_t libsais_unbwt_aux_ctx(const void *ctx, const uint8_t *T,
                                          uint8_t *U, int32_t *A, int32_t n,
                                          const int32_t *freq, int32_t r,
                                          const int32_t *I);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API int32_t libsais_unbwt_omp(const uint8_t *T, uint8_t *U, int32_t *A,
                                      int32_t n, const int32_t *freq, int32_t i,
                                      int32_t threads);

LIBSAIS_API int32_t libsais_unbwt_aux_omp(const uint8_t *T, uint8_t *U,
                                          int32_t *A, int32_t n,
                                          const int32_t *freq, int32_t r,
                                          const int32_t *I, int32_t threads);
#endif

LIBSAIS_API int32_t libsais_plcp(const uint8_t *T, const int32_t *SA,
                                 int32_t *PLCP, int32_t n);

LIBSAIS_API int32_t libsais_plcp_gsa(const uint8_t *T, const int32_t *SA,
                                     int32_t *PLCP, int32_t n);

LIBSAIS_API int32_t libsais_plcp_int(const int32_t *T, const int32_t *SA,
                                     int32_t *PLCP, int32_t n);

LIBSAIS_API int32_t libsais_lcp(const int32_t *PLCP, const int32_t *SA,
                                int32_t *LCP, int32_t n);

#if defined(LIBSAIS_OPENMP)

LIBSAIS_API int32_t libsais_plcp_omp(const uint8_t *T, const int32_t *SA,
                                     int32_t *PLCP, int32_t n, int32_t threads);

LIBSAIS_API int32_t libsais_plcp_gsa_omp(const uint8_t *T, const int32_t *SA,
                                         int32_t *PLCP, int32_t n,
                                         int32_t threads);

LIBSAIS_API int32_t libsais_plcp_int_omp(const int32_t *T, const int32_t *SA,
                                         int32_t *PLCP, int32_t n,
                                         int32_t threads);

LIBSAIS_API int32_t libsais_lcp_omp(const int32_t *PLCP, const int32_t *SA,
                                    int32_t *LCP, int32_t n, int32_t threads);
#endif

#ifdef __cplusplus
}
#endif

#endif
