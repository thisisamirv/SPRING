

/*--

This file is a part of bsc and/or libbsc, a program and a library for
lossless, block-sorting data compression.

   Copyright (c) 2009-2024 Ilya Grebnov <ilya.grebnov@gmail.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

Please see the file LICENSE for full copyright information and file AUTHORS
for full list of contributors.

See also the bsc and libbsc web site:
  http://libbsc.com/ for more information.

--*/

#ifndef _LIBBSC_FILTERS_H
#define _LIBBSC_FILTERS_H

#define LIBBSC_CONTEXTS_FOLLOWING 1
#define LIBBSC_CONTEXTS_PRECEDING 2

#ifndef LIBBSC_API
#ifdef _WIN32
#ifdef LIBBSC_SHARED
#ifdef LIBBSC_EXPORTS
#define LIBBSC_API __declspec(dllexport)
#else
#define LIBBSC_API __declspec(dllimport)
#endif
#else
#define LIBBSC_API
#endif
#else
#define LIBBSC_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

LIBBSC_API int bsc_detect_segments(const unsigned char *input, int n,
                                   int *segments, int k, int features);

LIBBSC_API int bsc_detect_contextsorder(const unsigned char *input, int n,
                                        int features);

LIBBSC_API int bsc_reverse_block(unsigned char *T, int n, int features);

LIBBSC_API int bsc_detect_recordsize(const unsigned char *input, int n,
                                     int features);

LIBBSC_API int bsc_reorder_forward(unsigned char *T, int n, int recordSize,
                                   int features);

LIBBSC_API int bsc_reorder_reverse(unsigned char *T, int n, int recordSize,
                                   int features);

#ifdef __cplusplus
}
#endif

#endif
