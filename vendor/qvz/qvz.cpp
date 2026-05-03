#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "qvz/cluster.h"
#include "qvz/codebook.h"
#include "qvz/qv_compressor.h"
#include "qvz/qvz.h"

namespace spring {
namespace qvz {

void encode(struct qv_options_t *opts, uint32_t max_readlen, uint32_t numreads,
            std::string *quality_string_array, uint32_t *str_len_array) {
  struct quality_file_t qv_info;
  memset(&qv_info, 0, sizeof(qv_info));

  for (int i = 0; i < 32; i++)
    qv_info.well.state[i] = i + 1;
  qv_info.well.n = 0;
  qv_info.well.bits_left = 0;

  struct distortion_t *dist;
  struct alphabet_t *alphabet = alloc_alphabet(ALPHABET_SIZE);

  if (opts->distortion == DISTORTION_CUSTOM) {
    dist = gen_custom_distortion(ALPHABET_SIZE, opts->dist_file);
  } else {
    dist = generate_distortion_matrix(ALPHABET_SIZE, opts->distortion);
  }

  qv_info.alphabet = alphabet;
  qv_info.dist = dist;
  qv_info.cluster_count = opts->clusters;
  qv_info.columns = max_readlen;
  qv_info.lines = numreads;

  qv_info.block_count = 1;
  qv_info.blocks = (struct line_block_t *)calloc(qv_info.block_count,
                                                 sizeof(struct line_block_t));
  qv_info.blocks[0].count = qv_info.lines;
  qv_info.blocks[0].quality_array = quality_string_array;
  qv_info.blocks[0].read_lengths = str_len_array;

  qv_info.clusters = alloc_cluster_list(&qv_info);
  qv_info.opts = opts;

  calculate_statistics(&qv_info);
  generate_codebooks(&qv_info);

  start_qv_quantization(&qv_info);
  free_blocks(&qv_info);
  free_cluster_list(qv_info.clusters);
  free_alphabet(alphabet);
  free_distortion_matrix(dist);
}

} // namespace qvz
} // namespace spring
