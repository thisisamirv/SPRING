

#include <stdlib.h>

#include "qvz/cluster.h"
#include "qvz/codebook.h"
#include "qvz/pmf.h"

namespace spring {
namespace qvz {

struct cluster_list_t *alloc_cluster_list(struct quality_file_t *info) {
  uint8_t j;
  struct cluster_list_t *rtn =
      (struct cluster_list_t *)calloc(1, sizeof(struct cluster_list_t));

  rtn->count = info->cluster_count;
  rtn->clusters =
      (struct cluster_t *)calloc(info->cluster_count, sizeof(struct cluster_t));
  rtn->distances = (double *)calloc(info->cluster_count, sizeof(double));

  for (j = 0; j < info->cluster_count; ++j) {
    rtn->clusters[j].id = j;
    rtn->clusters[j].count = 0;
    rtn->clusters[j].mean = (symbol_t *)calloc(info->columns, sizeof(symbol_t));
    rtn->clusters[j].accumulator =
        (uint64_t *)calloc(info->columns, sizeof(uint64_t));
    rtn->clusters[j].training_stats =
        alloc_conditional_pmf_list(info->alphabet, info->columns);
  }

  return rtn;
}

void free_cluster_list(struct cluster_list_t *clusters) {
  uint8_t j;

  for (j = 0; j < clusters->count; ++j) {
    free(clusters->clusters[j].mean);
    free(clusters->clusters[j].accumulator);
    free_conditional_pmf_list(clusters->clusters[j].training_stats);
    free_cond_quantizer_list(clusters->clusters[j].qlist);
  }
  free(clusters->distances);
  free(clusters->clusters);
  free(clusters);
}

} // namespace qvz
} // namespace spring
