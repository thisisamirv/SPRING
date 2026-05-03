#ifndef SPRING_QVZ_LINES_H_
#define SPRING_QVZ_LINES_H_

#include "qvz/distortion.h"
#include "qvz/pmf.h"
#include "qvz/well.h"
#include <stdint.h>
#include <string>

#define MAX_LINES_PER_BLOCK 1000000
#define MAX_READS_PER_LINE 1022
#define READ_LINEBUF_LENGTH (MAX_READS_PER_LINE + 2)

#define LF_ERROR_NONE 0
#define LF_ERROR_NOT_FOUND 1
#define LF_ERROR_NO_MEMORY 2
#define LF_ERROR_TOO_LONG 4

namespace spring {
namespace qvz {

struct line_block_t {
  uint32_t count;

  std::string *quality_array;
  uint32_t *read_lengths;
};

struct cluster_t {

  uint8_t id;
  uint32_t count;
  symbol_t *mean;
  uint64_t *accumulator;

  struct cond_pmf_list_t *training_stats;
  struct cond_quantizer_list_t *qlist;
};

struct cluster_list_t {
  uint8_t count;
  struct cluster_t *clusters;
  double *distances;
};

struct quality_file_t {
  struct alphabet_t *alphabet;
  char *path;
  uint64_t lines;
  uint32_t columns;
  uint32_t block_count;
  struct line_block_t *blocks;
  uint8_t cluster_count;
  struct cluster_list_t *clusters;
  struct distortion_t *dist;
  struct qv_options_t *opts;
  struct well_state_t well;
};

uint32_t load_file(const char *path, struct quality_file_t *info,
                   uint64_t max_lines);
uint32_t alloc_blocks(struct quality_file_t *info);
void free_blocks(struct quality_file_t *info);

} // namespace qvz
} // namespace spring

#endif
