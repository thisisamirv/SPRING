#ifndef SPRING_QVZ_CODEBOOK_H_
#define SPRING_QVZ_CODEBOOK_H_

#include "qvz/util.h"

#include <stdint.h>
#include <stdio.h>

#include "qvz/distortion.h"
#include "qvz/lines.h"
#include "qvz/pmf.h"
#include "qvz/quantizer.h"
#include "qvz/well.h"

namespace spring {
namespace qvz {

#define MODE_RATIO 0
#define MODE_FIXED 1
#define MODE_FIXED_MSE 2

struct qv_options_t {
  uint8_t verbose;
  uint8_t stats;
  uint8_t mode;
  uint8_t clusters;
  uint8_t uncompressed;
  uint8_t distortion;
  char *dist_file;
  char *uncompressed_name;
  double ratio;
  double e_dist;
  double cluster_threshold;
};

struct cond_pmf_list_t {
  uint32_t columns;
  const struct alphabet_t *alphabet;
  struct pmf_t **pmfs;
  struct pmf_list_t *marginal_pmfs;
};

struct cond_quantizer_list_t {
  uint32_t columns;
  uint32_t lines;
  struct alphabet_t **input_alphabets;
  struct quantizer_t ***q;
  double **ratio;
  uint8_t **qratio;
  struct qv_options_t *options;
};

struct cond_pmf_list_t *
alloc_conditional_pmf_list(const struct alphabet_t *alphabet, uint32_t columns);
struct cond_quantizer_list_t *
alloc_conditional_quantizer_list(uint32_t columns);
void free_conditional_pmf_list(struct cond_pmf_list_t *);
void free_cond_quantizer_list(struct cond_quantizer_list_t *);

void cond_quantizer_init_column(struct cond_quantizer_list_t *list,
                                uint32_t column,
                                const struct alphabet_t *input_union);

struct pmf_t *get_cond_pmf(struct cond_pmf_list_t *list, uint32_t column,
                           symbol_t prev);
struct quantizer_t *
get_cond_quantizer_indexed(struct cond_quantizer_list_t *list, uint32_t column,
                           uint32_t index);
struct quantizer_t *get_cond_quantizer(struct cond_quantizer_list_t *list,
                                       uint32_t column, symbol_t prev);
void store_cond_quantizers(struct quantizer_t *restrict lo,
                           struct quantizer_t *restrict hi, double ratio,
                           struct cond_quantizer_list_t *list, uint32_t column,
                           symbol_t prev);
void store_cond_quantizers_indexed(struct quantizer_t *restrict lo,
                                   struct quantizer_t *restrict hi,
                                   double ratio,
                                   struct cond_quantizer_list_t *list,
                                   uint32_t column, uint32_t index);
struct quantizer_t *choose_quantizer(struct cond_quantizer_list_t *list,
                                     struct well_state_t *well, uint32_t column,
                                     symbol_t prev, uint32_t *q_idx);
uint32_t find_state_encoding(struct quantizer_t *codebook, symbol_t value);

void calculate_statistics(struct quality_file_t *);
double optimize_for_entropy(struct pmf_t *pmf, struct distortion_t *dist,
                            double target, struct quantizer_t **lo,
                            struct quantizer_t **hi);
void generate_codebooks(struct quality_file_t *info);

void write_codebooks(FILE *fp, struct quality_file_t *info);
void write_codebook(FILE *fp, struct cond_quantizer_list_t *quantizers);
void read_codebooks(FILE *fp, struct quality_file_t *info);
struct cond_quantizer_list_t *read_codebook(FILE *fp,
                                            struct quality_file_t *info);

#define MAX_CODEBOOK_LINE_LENGTH 3366
#define COPY_Q_TO_LINE(line, q, i, size)                                       \
  for (i = 0; i < size; ++i) {                                                 \
    line[i] = q[i] + 33;                                                       \
  }
#define COPY_Q_FROM_LINE(line, q, i, size)                                     \
  for (i = 0; i < size; ++i) {                                                 \
    q[i] = line[i] - 33;                                                       \
  }

void print_codebook(struct cond_quantizer_list_t *);

} // namespace qvz
} // namespace spring
#endif
