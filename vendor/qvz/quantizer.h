#ifndef SPRING_QVZ_QUANTIZER_H_
#define SPRING_QVZ_QUANTIZER_H_

#include <stdint.h>

#include "qvz/distortion.h"
#include "qvz/pmf.h"
#include "qvz/util.h"

#define QUANTIZER_MAX_ITER 100

namespace spring {
namespace qvz {

struct quantizer_t {
  const struct alphabet_t *restrict alphabet;
  struct alphabet_t *restrict output_alphabet;
  uint32_t *restrict q;
  double ratio;
  double mse;
};

struct quantizer_t *alloc_quantizer(const struct alphabet_t *);
void free_quantizer(struct quantizer_t *);

struct quantizer_t *generate_quantizer(struct pmf_t *restrict pmf,
                                       struct distortion_t *restrict dist,
                                       uint32_t states);

struct pmf_t *apply_quantizer(struct quantizer_t *restrict q,
                              struct pmf_t *restrict pmf,
                              struct pmf_t *restrict output);

void find_output_alphabet(struct quantizer_t *);

void print_quantizer(struct quantizer_t *);

} // namespace qvz
} // namespace spring

#endif
