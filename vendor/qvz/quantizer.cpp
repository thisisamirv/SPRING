#include <limits>
#include <stdio.h>
#include <string.h>

#include "qvz/quantizer.h"
#include "qvz/util.h"

namespace spring {
namespace qvz {

struct quantizer_t *alloc_quantizer(const struct alphabet_t *alphabet) {
  struct quantizer_t *rtn =
      (struct quantizer_t *)calloc(1, sizeof(struct quantizer_t));
  rtn->alphabet = alphabet;
  rtn->q = (uint32_t *)calloc(alphabet->size, sizeof(uint32_t));
  return rtn;
}

void free_quantizer(struct quantizer_t *q) {
  if (q->output_alphabet)
    free_alphabet(q->output_alphabet);

  free(q->q);
  free(q);
}

struct quantizer_t *generate_quantizer(struct pmf_t *restrict pmf,
                                       struct distortion_t *restrict dist,
                                       uint32_t states) {
  struct quantizer_t *q = alloc_quantizer(pmf->alphabet);
  uint32_t changed = 1;
  uint32_t iter = 0;
  uint32_t i, j, r, size;
  uint32_t min_r;
  double mse, min_mse, next_mse;
  uint32_t *bounds = (uint32_t *)malloc((states + 1) * sizeof(uint32_t));
  uint32_t *reconstruction = (uint32_t *)malloc(states * sizeof(uint32_t));

  bounds[0] = 0;
  bounds[states] = pmf->alphabet->size;
  for (j = 1; j < states; ++j) {
    bounds[j] = (j * pmf->alphabet->size) / states;
  }
  for (j = 0; j < states; ++j) {
    reconstruction[j] = (bounds[j] + bounds[j + 1] - 1) / 2;
  }

  size = pmf->alphabet->size;
  double *p_sum = (double *)malloc((size + 1) * sizeof(double));
  double *pi_sum = (double *)malloc((size + 1) * sizeof(double));
  double *pi2_sum = (double *)malloc((size + 1) * sizeof(double));
  p_sum[0] = 0;
  pi_sum[0] = 0;
  pi2_sum[0] = 0;
  for (i = 0; i < size; ++i) {
    double p = get_probability(pmf, i);
    p_sum[i + 1] = p_sum[i] + p;
    pi_sum[i + 1] = pi_sum[i] + (double)i * p;
    pi2_sum[i + 1] = pi2_sum[i] + (double)i * i * p;
  }

  while (changed && iter < QUANTIZER_MAX_ITER) {
    changed = 0;
    iter += 1;

    for (j = 0; j < states; ++j) {

      min_mse = std::numeric_limits<double>::max();
      min_r = bounds[j];

      uint32_t start = bounds[j];
      uint32_t end = bounds[j + 1];

      if (dist->type == DISTORTION_MSE) {
        double s0 = p_sum[end] - p_sum[start];
        double s1 = pi_sum[end] - pi_sum[start];
        double s2 = pi2_sum[end] - pi2_sum[start];
        for (r = start; r < end; ++r) {
          mse = s2 - 2.0 * r * s1 + (double)r * r * s0;
          if (mse < min_mse) {
            min_r = r;
            min_mse = mse;
          }
        }
      } else if (dist->type == DISTORTION_MANHATTAN) {
        for (r = start; r < end; ++r) {
          double s0_lo = p_sum[r + 1] - p_sum[start];
          double s1_lo = pi_sum[r + 1] - pi_sum[start];
          double s0_hi = p_sum[end] - p_sum[r + 1];
          double s1_hi = pi_sum[end] - pi_sum[r + 1];
          mse = (r * s0_lo - s1_lo) + (s1_hi - r * s0_hi);
          if (mse < min_mse) {
            min_r = r;
            min_mse = mse;
          }
        }
      } else {

        for (r = start; r < end; ++r) {
          mse = 0.0;
          for (i = start; i < end; ++i) {
            mse += get_probability(pmf, i) * get_distortion(dist, i, r);
          }
          if (mse < min_mse) {
            min_r = r;
            min_mse = mse;
          }
        }
      }

      if (min_r != reconstruction[j]) {
        changed = 1;
        reconstruction[j] = min_r;
      }
    }

    r = 0;
    for (j = 1; j < size - 1 && r < states - 1; ++j) {

      mse = get_distortion(dist, j, reconstruction[r]);
      next_mse = get_distortion(dist, j, reconstruction[r + 1]);

      if (next_mse < mse) {
        r += 1;
        bounds[r] = j;
      }
    }
  }

  for (j = 0; j < states; ++j) {
    for (i = bounds[j]; i < bounds[j + 1]; ++i) {
      q->q[i] = reconstruction[j];
    }
  }

  q->output_alphabet = alloc_alphabet(states);
  for (j = 0; j < states; ++j) {
    q->output_alphabet->symbols[j] = (symbol_t)reconstruction[j];
  }
  alphabet_compute_index(q->output_alphabet);

  q->mse = 0.0;
  for (j = 0; j < states; ++j) {
    uint32_t start = bounds[j];
    uint32_t end = bounds[j + 1];
    uint32_t r_pt = reconstruction[j];

    if (dist->type == DISTORTION_MSE) {
      double s0 = p_sum[end] - p_sum[start];
      double s1 = pi_sum[end] - pi_sum[start];
      double s2 = pi2_sum[end] - pi2_sum[start];
      q->mse += s2 - 2.0 * r_pt * s1 + (double)r_pt * r_pt * s0;
    } else if (dist->type == DISTORTION_MANHATTAN) {
      double s0_lo = p_sum[r_pt + 1] - p_sum[start];
      double s1_lo = pi_sum[r_pt + 1] - pi_sum[start];
      double s0_hi = p_sum[end] - p_sum[r_pt + 1];
      double s1_hi = pi_sum[end] - pi_sum[r_pt + 1];
      q->mse += (r_pt * s0_lo - s1_lo) + (s1_hi - r_pt * s0_hi);
    } else {
      for (i = start; i < end; ++i) {
        q->mse += get_distortion(dist, i, r_pt) * get_probability(pmf, i);
      }
    }
  }
  free(p_sum);
  free(pi_sum);
  free(pi2_sum);
  free(bounds);
  free(reconstruction);
  return q;
}

struct pmf_t *apply_quantizer(struct quantizer_t *restrict q,
                              struct pmf_t *restrict pmf,
                              struct pmf_t *restrict output) {
  uint32_t i;

  if (!pmf->pmf_ready)
    recalculate_pmf(pmf);

  if (output) {

    memset(output->pmf, 0, output->alphabet->size * sizeof(double));
  } else {

    output = alloc_pmf(pmf->alphabet);
  }

  for (i = 0; i < pmf->alphabet->size; ++i) {
    output->pmf[q->q[i]] += get_probability(pmf, i);
  }
  output->pmf_ready = 1;

  return output;
}

void find_output_alphabet(struct quantizer_t *q) {
  symbol_t p;
  uint32_t x;
  uint32_t size;
  symbol_t *uniques = (symbol_t *)malloc(q->alphabet->size * sizeof(symbol_t));

  p = q->q[0];
  uniques[0] = p;
  size = 1;

  for (x = 1; x < q->alphabet->size; ++x) {
    if (q->q[x] != p) {
      p = q->q[x];
      uniques[size] = p;
      size += 1;
    }
  }

  q->output_alphabet = alloc_alphabet(size);
  memcpy(q->output_alphabet->symbols, uniques, size * sizeof(symbol_t));
  alphabet_compute_index(q->output_alphabet);
  free(uniques);
}

void print_quantizer(struct quantizer_t *q) {
  uint32_t i;
  char *tmp = (char *)malloc(q->alphabet->size + 1);

  tmp[q->alphabet->size] = 0;
  for (i = 0; i < q->alphabet->size; ++i) {
    tmp[i] = (char)(q->q[i] + 33);
  }
  printf("Quantizer: %s\n", tmp);

  tmp[q->output_alphabet->size] = 0;
  for (i = 0; i < q->output_alphabet->size; ++i) {
    tmp[i] = (char)(q->output_alphabet->symbols[i] + 33);
  }
  printf("Unique alphabet: %s\n", tmp);
  free(tmp);
}

} // namespace qvz
} // namespace spring
