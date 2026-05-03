

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

#ifndef _LIBBSC_QLFC_H
#define _LIBBSC_QLFC_H

#ifdef __cplusplus
extern "C" {
#endif

int bsc_qlfc_init(int features);

int bsc_qlfc_static_encode_block(const unsigned char *input,
                                 unsigned char *output, int inputSize,
                                 int outputSize);

int bsc_qlfc_adaptive_encode_block(const unsigned char *input,
                                   unsigned char *output, int inputSize,
                                   int outputSize);

int bsc_qlfc_fast_encode_block(const unsigned char *input,
                               unsigned char *output, int inputSize,
                               int outputSize);

int bsc_qlfc_static_decode_block(const unsigned char *input,
                                 unsigned char *output);

int bsc_qlfc_adaptive_decode_block(const unsigned char *input,
                                   unsigned char *output);

int bsc_qlfc_fast_decode_block(const unsigned char *input,
                               unsigned char *output);

#ifdef __cplusplus
}
#endif

#endif
