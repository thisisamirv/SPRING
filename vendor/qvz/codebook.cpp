#include "qvz/codebook.h"
#include "qvz/cluster.h"
#include "qvz/lines.h"

#include <assert.h>
#include <stdio.h>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
namespace spring {
namespace qvz {

struct cond_pmf_list_t *
alloc_conditional_pmf_list(const struct alphabet_t *alphabet,
                           uint32_t columns) {
  uint32_t count = 1 + alphabet->size * (columns - 1);
  uint32_t i;
  struct cond_pmf_list_t *list =
      (struct cond_pmf_list_t *)calloc(1, sizeof(struct cond_pmf_list_t));

  list->columns = columns;
  list->alphabet = alphabet;
  list->pmfs = (struct pmf_t **)calloc(count, sizeof(struct pmf_t *));

  for (i = 0; i < count; ++i) {
    list->pmfs[i] = alloc_pmf(alphabet);
  }

  return list;
}

void free_conditional_pmf_list(struct cond_pmf_list_t *list) {
  uint32_t count = 1 + list->alphabet->size * (list->columns - 1);
  uint32_t i;

  for (i = 0; i < count; ++i) {
    free_pmf(list->pmfs[i]);
  }
  free(list->pmfs);
  free_pmf_list(list->marginal_pmfs);
  free(list);
}

struct cond_quantizer_list_t *
alloc_conditional_quantizer_list(uint32_t columns) {
  struct cond_quantizer_list_t *rtn = (struct cond_quantizer_list_t *)calloc(
      1, sizeof(struct cond_quantizer_list_t));
  rtn->columns = columns;
  rtn->input_alphabets =
      (struct alphabet_t **)calloc(columns, sizeof(struct alphabet_t *));
  rtn->q =
      (struct quantizer_t ***)calloc(columns, sizeof(struct quantizer_t **));
  rtn->ratio = (double **)calloc(columns, sizeof(double *));
  rtn->qratio = (uint8_t **)calloc(columns, sizeof(uint8_t *));
  return rtn;
}

void free_cond_quantizer_list(struct cond_quantizer_list_t *list) {
  uint32_t i, j;

  for (i = 0; i < list->columns; ++i) {
    if (list->q[i]) {
      for (j = 0; j < 2 * list->input_alphabets[i]->size; ++j) {
        if (list->q[i][j])
          free_quantizer(list->q[i][j]);
      }
      free_alphabet(list->input_alphabets[i]);
      free(list->q[i]);
      free(list->ratio[i]);
      free(list->qratio[i]);
    }
  }

  free(list->qratio);
  free(list->ratio);
  free(list->q);
  free(list->input_alphabets);
  free(list);
}

void cond_quantizer_init_column(struct cond_quantizer_list_t *list,
                                uint32_t column,
                                const struct alphabet_t *input_union) {
  list->input_alphabets[column] = duplicate_alphabet(input_union);

  list->q[column] = (struct quantizer_t **)calloc(input_union->size * 2,
                                                  sizeof(struct quantizer_t *));

  list->ratio[column] = (double *)calloc(input_union->size, sizeof(double));
  list->qratio[column] = (uint8_t *)calloc(input_union->size, sizeof(uint8_t));
}

struct pmf_t *get_cond_pmf(struct cond_pmf_list_t *list, uint32_t column,
                           symbol_t prev) {
  if (column == 0)
    return list->pmfs[0];
  return list->pmfs[1 + (column - 1) * list->alphabet->size + prev];
}

struct quantizer_t *
get_cond_quantizer_indexed(struct cond_quantizer_list_t *list, uint32_t column,
                           uint32_t index) {
  return list->q[column][index];
}

struct quantizer_t *get_cond_quantizer(struct cond_quantizer_list_t *list,
                                       uint32_t column, symbol_t prev) {
  uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
  if (idx != ALPHABET_SYMBOL_NOT_FOUND)
    return get_cond_quantizer_indexed(list, column, idx);
  return NULL;
}

void store_cond_quantizers(struct quantizer_t *restrict lo,
                           struct quantizer_t *restrict hi, double ratio,
                           struct cond_quantizer_list_t *list, uint32_t column,
                           symbol_t prev) {
  uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
  store_cond_quantizers_indexed(lo, hi, ratio, list, column, idx);
}

void store_cond_quantizers_indexed(struct quantizer_t *restrict lo,
                                   struct quantizer_t *restrict hi,
                                   double ratio,
                                   struct cond_quantizer_list_t *list,
                                   uint32_t column, uint32_t idx) {
  list->q[column][2 * idx] = lo;
  list->q[column][2 * idx + 1] = hi;
  list->ratio[column][idx] = ratio;
  list->qratio[column][idx] = (uint8_t)(ratio * 128.);
}

struct quantizer_t *choose_quantizer(struct cond_quantizer_list_t *list,
                                     struct well_state_t *well, uint32_t column,
                                     symbol_t prev, uint32_t *q_idx) {
  uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
  assert(idx != ALPHABET_SYMBOL_NOT_FOUND);
  if (well_1024a_bits(well, 7) >= list->qratio[column][idx]) {
    *q_idx = 2 * idx + 1;
    return list->q[column][2 * idx + 1];
  }
  *q_idx = 2 * idx;
  return list->q[column][2 * idx];
}

uint32_t find_state_encoding(struct quantizer_t *q, symbol_t value) {
  return get_symbol_index(q->output_alphabet, value);
}

void calculate_statistics(struct quality_file_t *info) {
  uint32_t block, line_idx, column;
  uint32_t j;
  uint8_t c;
  uint32_t cur_readlen;
  char *line;
  struct cluster_t *cluster;
  struct cond_pmf_list_t *pmf_list;

  for (block = 0; block < info->block_count; ++block) {
    for (line_idx = 0; line_idx < info->blocks[block].count; ++line_idx) {
      line = &(info->blocks[block].quality_array[line_idx][0]);
      cur_readlen = info->blocks[block].read_lengths[line_idx];

      cluster = &info->clusters->clusters[0];

      pmf_list = cluster->training_stats;

      if (cur_readlen > 0) {
        pmf_increment(get_cond_pmf(pmf_list, 0, 0), line[0] - 33);
        for (column = 1; column < cur_readlen; ++column) {
          pmf_increment(get_cond_pmf(pmf_list, column, line[column - 1] - 33),
                        line[column] - 33);
        }
      }
    }
  }

  for (c = 0; c < info->cluster_count; ++c) {
    cluster = &info->clusters->clusters[c];
    pmf_list = cluster->training_stats;

    pmf_list->marginal_pmfs = alloc_pmf_list(info->columns, pmf_list->alphabet);
    combine_pmfs(get_cond_pmf(pmf_list, 0, 0), pmf_list->marginal_pmfs->pmfs[0],
                 1.0, 0.0, pmf_list->marginal_pmfs->pmfs[0]);
    for (column = 1; column < info->columns; ++column) {
      for (j = 0; j < pmf_list->alphabet->size; ++j) {
        combine_pmfs(
            pmf_list->marginal_pmfs->pmfs[column],
            get_cond_pmf(pmf_list, column, j), 1.0,
            get_probability(pmf_list->marginal_pmfs->pmfs[column - 1], j),
            pmf_list->marginal_pmfs->pmfs[column]);
      }
    }
  }
}

double optimize_for_entropy(struct pmf_t *pmf, struct distortion_t *dist,
                            double target, struct quantizer_t **lo,
                            struct quantizer_t **hi) {
  struct quantizer_t *q_temp;
  double lo_entropy, hi_entropy;
  struct pmf_t *pmf_temp = alloc_pmf(pmf->alphabet);

  double source_entropy = get_entropy(pmf);
  if (target >= source_entropy || target <= 0.0) {
    uint32_t final_states = (target <= 0.0) ? 1 : pmf->alphabet->size;
    *lo = generate_quantizer(pmf, dist, final_states);
    *hi = generate_quantizer(pmf, dist, final_states);
    free_pmf(pmf_temp);
    return 1.0;
  }

  uint32_t low = 1;
  uint32_t high = pmf->alphabet->size;
  lo_entropy = 0.0;
  hi_entropy = source_entropy;

  while (high - low > 1) {
    uint32_t mid = low + (high - low) / 2;
    q_temp = generate_quantizer(pmf, dist, mid);
    double mid_entropy = get_entropy(apply_quantizer(q_temp, pmf, pmf_temp));

    if (mid_entropy < target) {
      low = mid;
      lo_entropy = mid_entropy;
    } else {
      high = mid;
      hi_entropy = mid_entropy;
    }
    free_quantizer(q_temp);
  }

  *lo = generate_quantizer(pmf, dist, low);
  *hi = generate_quantizer(pmf, dist, high);
  lo_entropy = get_entropy(apply_quantizer(*lo, pmf, pmf_temp));
  hi_entropy = get_entropy(apply_quantizer(*hi, pmf, pmf_temp));

  free_pmf(pmf_temp);

  if (hi_entropy < target)
    return 0.0;
  else if (lo_entropy >= target || hi_entropy == lo_entropy)
    return 1.0;
  else
    return (target - hi_entropy) / (lo_entropy - hi_entropy);
}

void compute_qpmf_quan_list(struct quantizer_t *q_lo, struct quantizer_t *q_hi,
                            struct pmf_list_t *q_x_pmf, double ratio,
                            struct alphabet_t *q_output_union) {
  symbol_t x;
  uint32_t q_symbol, idx;

  for (x = 0; x < q_lo->alphabet->size; x++) {
    for (idx = 0; idx < q_output_union->size; idx++) {
      q_symbol = q_output_union->symbols[idx];

      if (q_lo->q[(uint8_t)x] == q_symbol)
        q_x_pmf->pmfs[(uint8_t)x]->pmf[idx] += ratio;

      if (q_hi->q[(uint8_t)x] == q_symbol)
        q_x_pmf->pmfs[(uint8_t)x]->pmf[idx] += (1 - ratio);
    }
  }
}

void compute_qpmf_list(struct pmf_list_t *qpmf_list,
                       struct cond_pmf_list_t *in_pmfs, uint32_t column,
                       struct pmf_list_t *prev_qpmf_list,
                       struct alphabet_t *q_alphabet_union,
                       struct alphabet_t *prev_q_alphabet_union,
                       struct cond_quantizer_list_t *q_list) {
  symbol_t x;
  uint32_t k, j;
  struct quantizer_t *q_hi, *q_lo;

  for (k = 0; k < qpmf_list->size; k++) {

    for (j = 0; j < prev_q_alphabet_union->size; j++) {

      double p_temp = 0.0;
      for (x = 0; x < prev_qpmf_list->size; ++x) {
        p_temp += get_probability(prev_qpmf_list->pmfs[(uint8_t)x], j) *
                  get_probability(get_cond_pmf(in_pmfs, column - 1, x), k) *
                  get_probability(in_pmfs->marginal_pmfs->pmfs[column - 2], x);
      }
      if (p_temp == 0.0)
        continue;

      q_lo = get_cond_quantizer_indexed(q_list, column - 1, 2 * j);
      q_hi = get_cond_quantizer_indexed(q_list, column - 1, (2 * j) + 1);

      uint32_t idx_lo =
          get_symbol_index(q_alphabet_union, (symbol_t)q_lo->q[k]);
      uint32_t idx_hi =
          get_symbol_index(q_alphabet_union, (symbol_t)q_hi->q[k]);

      if (idx_lo != ALPHABET_SYMBOL_NOT_FOUND)
        qpmf_list->pmfs[k]->pmf[idx_lo] += q_lo->ratio * p_temp;

      if (idx_hi != ALPHABET_SYMBOL_NOT_FOUND)
        qpmf_list->pmfs[k]->pmf[idx_hi] += q_hi->ratio * p_temp;
    }

    qpmf_list->pmfs[k]->pmf_ready = 1;
    renormalize_pmf(qpmf_list->pmfs[k]);
  }
}

void compute_xpmf_list(struct pmf_list_t *qpmf_list,
                       struct cond_pmf_list_t *in_pmfs, uint32_t column,
                       struct pmf_list_t *xpmf_list,
                       struct alphabet_t *q_alphabet_union) {
  symbol_t x;
  uint32_t idx, k;

  for (idx = 0; idx < q_alphabet_union->size; idx++) {

    for (k = 0; k < qpmf_list->size; k++) {

      for (x = 0; x < qpmf_list->size; ++x) {
        xpmf_list->pmfs[idx]->pmf[k] +=
            get_probability(qpmf_list->pmfs[(uint8_t)x], idx) *
            get_probability(get_cond_pmf(in_pmfs, column, x), k) *
            get_probability(in_pmfs->marginal_pmfs->pmfs[column - 1], x);
      }
    }

    xpmf_list->pmfs[idx]->pmf_ready = 1;
    renormalize_pmf(xpmf_list->pmfs[idx]);
  }
}

void generate_codebooks(struct quality_file_t *info) {

  double ratio;

  uint32_t column, j;
  double total_mse;

  struct cond_quantizer_list_t *q_list;

  const struct alphabet_t *A = info->alphabet;

  struct quantizer_t *q_lo;
  struct quantizer_t *q_hi;

  struct pmf_list_t *xpmf_list;

  struct pmf_list_t *qpmf_list;
  struct pmf_list_t *prev_qpmf_list;

  struct alphabet_t *q_output_union;
  struct alphabet_t *q_prev_output_union;

  uint8_t cluster_id;
  struct cond_pmf_list_t *in_pmfs;
  struct qv_options_t *opts = info->opts;
  struct distortion_t *dist = info->dist;

  for (cluster_id = 0; cluster_id < info->cluster_count; ++cluster_id) {
    q_list = alloc_conditional_quantizer_list(info->columns);
    info->clusters->clusters[cluster_id].qlist = q_list;
    in_pmfs = info->clusters->clusters[cluster_id].training_stats;

    q_output_union = alloc_alphabet(1);
    cond_quantizer_init_column(q_list, 0, q_output_union);
    q_list->options = opts;

    qpmf_list = alloc_pmf_list(A->size, q_output_union);

    if (opts->mode == MODE_RATIO)
      ratio = optimize_for_entropy(
          get_cond_pmf(in_pmfs, 0, 0), dist,
          get_entropy(get_cond_pmf(in_pmfs, 0, 0)) * opts->ratio, &q_lo, &q_hi);
    else
      ratio = optimize_for_entropy(get_cond_pmf(in_pmfs, 0, 0), dist,
                                   opts->ratio, &q_lo, &q_hi);
    q_lo->ratio = ratio;
    q_hi->ratio = 1 - ratio;
    total_mse = ratio * q_lo->mse + (1 - ratio) * q_hi->mse;
    store_cond_quantizers(q_lo, q_hi, ratio, q_list, 0, 0);

    q_prev_output_union = q_output_union;
    prev_qpmf_list = qpmf_list;

    for (column = 1; column < info->columns; column++) {

      q_output_union = duplicate_alphabet(
          get_cond_quantizer_indexed(q_list, column - 1, 0)->output_alphabet);
      for (j = 1; j < 2 * q_prev_output_union->size; ++j) {
        struct alphabet_t *temp_union = alloc_alphabet(0);
        alphabet_union(
            q_output_union,
            get_cond_quantizer_indexed(q_list, column - 1, j)->output_alphabet,
            temp_union);
        free_alphabet(q_output_union);
        q_output_union = temp_union;
      }
      cond_quantizer_init_column(q_list, column, q_output_union);

      qpmf_list = alloc_pmf_list(A->size, q_output_union);
      xpmf_list = alloc_pmf_list(q_output_union->size, A);

      if (column == 1)
        compute_qpmf_quan_list(q_lo, q_hi, qpmf_list, ratio, q_output_union);
      else
        compute_qpmf_list(qpmf_list, in_pmfs, column, prev_qpmf_list,
                          q_output_union, q_prev_output_union, q_list);

      compute_xpmf_list(qpmf_list, in_pmfs, column, xpmf_list, q_output_union);

      for (j = 0; j < q_output_union->size; ++j) {

        if (opts->mode == MODE_RATIO)
          ratio = optimize_for_entropy(
              xpmf_list->pmfs[j], dist,
              get_entropy(xpmf_list->pmfs[j]) * opts->ratio, &q_lo, &q_hi);
        else
          ratio = optimize_for_entropy(xpmf_list->pmfs[j], dist, opts->ratio,
                                       &q_lo, &q_hi);
        q_lo->ratio = ratio;
        q_hi->ratio = 1 - ratio;
        store_cond_quantizers_indexed(q_lo, q_hi, ratio, q_list, column, j);

        total_mse += (ratio * q_lo->mse + (1 - ratio) * q_hi->mse) /
                     q_output_union->size;
      }

      free_alphabet(q_prev_output_union);
      q_prev_output_union = q_output_union;
      free_pmf_list(prev_qpmf_list);
      prev_qpmf_list = qpmf_list;
      free_pmf_list(xpmf_list);
    }

    free_pmf_list(qpmf_list);
    free_alphabet(q_output_union);
  }
}

void write_codebooks(FILE *fp, struct quality_file_t *info) {
  uint32_t columns, lines;
  uint32_t j;
  char linebuf[1];

  columns = htonl(info->columns);
  lines = htonl((uint32_t)info->lines);
  linebuf[0] = info->cluster_count;
  fwrite(linebuf, sizeof(char), 1, fp);
  fwrite(&columns, sizeof(uint32_t), 1, fp);
  fwrite(&lines, sizeof(uint32_t), 1, fp);

  for (j = 0; j < info->cluster_count; ++j) {
    write_codebook(fp, info->clusters->clusters[j].qlist);
  }
}

void write_codebook(FILE *fp, struct cond_quantizer_list_t *quantizers) {
  uint32_t i, j, k;
  uint32_t columns = quantizers->columns;
  struct quantizer_t *q_temp = get_cond_quantizer_indexed(quantizers, 0, 0);
  uint32_t size = q_temp->alphabet->size;
  uint32_t buflen = columns > size ? columns : size;
  char const *eol = "\n";
  char *linebuf = (char *)malloc(sizeof(char) * buflen);

  linebuf[0] = quantizers->qratio[0][0] + 33;
  linebuf[1] = eol[0];
  fwrite(linebuf, sizeof(char), 2, fp);

  COPY_Q_TO_LINE(linebuf, q_temp->q, i, size);
  fwrite(linebuf, sizeof(char), size, fp);
  fwrite(eol, sizeof(char), 1, fp);

  q_temp = get_cond_quantizer_indexed(quantizers, 0, 1);
  COPY_Q_TO_LINE(linebuf, q_temp->q, i, size);
  fwrite(linebuf, sizeof(char), size, fp);
  fwrite(eol, sizeof(char), 1, fp);

  for (i = 1; i < columns; ++i) {

    for (j = 0; j < quantizers->input_alphabets[i]->size; ++j) {
      linebuf[j] = quantizers->qratio[i][j] + 33;
    }
    fwrite(linebuf, sizeof(char), quantizers->input_alphabets[i]->size, fp);
    fwrite(eol, sizeof(char), 1, fp);

    for (j = 0; j < quantizers->input_alphabets[i]->size; ++j) {
      q_temp = get_cond_quantizer_indexed(quantizers, i, 2 * j);
      COPY_Q_TO_LINE(linebuf, q_temp->q, k, size);
      fwrite(linebuf, sizeof(char), size, fp);
    }
    fwrite(eol, sizeof(char), 1, fp);

    for (j = 0; j < quantizers->input_alphabets[i]->size; ++j) {
      q_temp = get_cond_quantizer_indexed(quantizers, i, 2 * j + 1);
      COPY_Q_TO_LINE(linebuf, q_temp->q, k, size);
      fwrite(linebuf, sizeof(char), size, fp);
    }
    fwrite(eol, sizeof(char), 1, fp);
  }
  free(linebuf);
}

void read_codebooks(FILE *fp, struct quality_file_t *info) {
  uint8_t j;
  char line[9];

  fread(line, sizeof(char), 9, fp);
  info->cluster_count = line[0];

  info->columns = (line[1] & 0xff) | ((line[2] << 8) & 0xff00) |
                  ((line[3] << 16) & 0xff0000) | ((line[4] << 24) & 0xff000000);
  info->columns = ntohl(info->columns);
  info->lines = (line[5] & 0xff) | ((line[6] << 8) & 0xff00) |
                ((line[7] << 16) & 0xff0000) | ((line[8] << 24) & 0xff000000);
  info->lines = ntohl(info->lines);

  info->clusters = alloc_cluster_list(info);

  for (j = 0; j < info->cluster_count; ++j) {
    info->clusters->clusters[j].qlist = read_codebook(fp, info);
  }
}

struct cond_quantizer_list_t *read_codebook(FILE *fp,
                                            struct quality_file_t *info) {
  uint32_t column, size;
  uint32_t i, j;
  struct quantizer_t *q_lo, *q_hi;
  struct cond_quantizer_list_t *qlist;
  struct alphabet_t *uniques;
  char line[MAX_CODEBOOK_LINE_LENGTH];
  uint8_t qratio;
  struct alphabet_t *A = info->alphabet;
  uint32_t columns = info->columns;

  uniques = alloc_alphabet(1);
  qlist = alloc_conditional_quantizer_list(info->columns);
  cond_quantizer_init_column(qlist, 0, uniques);
  free_alphabet(uniques);

  fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
  qratio = line[0] - 33;

  q_lo = alloc_quantizer(A);
  q_hi = alloc_quantizer(A);
  fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
  COPY_Q_FROM_LINE(line, q_lo->q, j, A->size);
  fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
  COPY_Q_FROM_LINE(line, q_hi->q, j, A->size);

  find_output_alphabet(q_lo);
  find_output_alphabet(q_hi);
  uniques = alloc_alphabet(0);
  alphabet_union(q_lo->output_alphabet, q_hi->output_alphabet, uniques);
  store_cond_quantizers_indexed(q_lo, q_hi, 0.0, qlist, 0, 0);
  qlist->qratio[0][0] = qratio;

  for (column = 1; column < columns; ++column) {

    cond_quantizer_init_column(qlist, column, uniques);
    size = uniques->size;
    free_alphabet(uniques);
    uniques = alloc_alphabet(0);

    fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
    for (i = 0; i < size; ++i) {
      qlist->qratio[column][i] = line[i] - 33;
    }

    for (i = 0; i < size; ++i) {
      q_lo = alloc_quantizer(A);
      fread(line, A->size * sizeof(symbol_t), 1, fp);
      COPY_Q_FROM_LINE(line, q_lo->q, j, A->size);

      find_output_alphabet(q_lo);
      qlist->q[column][2 * i] = q_lo;
      alphabet_union(uniques, q_lo->output_alphabet, uniques);
    }

    (void)fgets(line, 2, fp);

    for (i = 0; i < size; ++i) {
      q_hi = alloc_quantizer(A);
      fread(line, A->size * sizeof(symbol_t), 1, fp);
      COPY_Q_FROM_LINE(line, q_hi->q, j, A->size);

      find_output_alphabet(q_hi);
      qlist->q[column][2 * i + 1] = q_hi;
      alphabet_union(uniques, q_hi->output_alphabet, uniques);
    }

    (void)fgets(line, 2, fp);
  }

  free_alphabet(uniques);

  return qlist;
}

void print_codebook(struct cond_quantizer_list_t *q) {
  struct alphabet_t *A;
  uint32_t j;
  uint32_t column;

  for (column = 0; column < q->columns; ++column) {
    A = q->input_alphabets[column];
    for (j = 0; j < 2 * A->size; ++j) {
      print_quantizer(q->q[column][j]);
    }
  }
}

} // namespace qvz
} // namespace spring
