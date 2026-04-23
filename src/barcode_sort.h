// barcode_sort.h — Barcode-first read ordering for single-cell assays.
//
// For sc-rna / sc-atac / sc-methyl assays, the standard k-mer overlap
// reordering engine is counterproductive: it randomises barcode order so the
// barcode stream compresses as near-random noise.
//
// barcode_sort() is a drop-in replacement for call_reorder().  It produces the
// exact same output file schema (temp.dna.N, read_rev.txt.N, tempflag.txt.N,
// temppos.txt.N, read_order.bin.N, read_lengths.bin.N, plus the merged
// singleton stubs) so the encoder pipeline needs no changes.
//
// Design rules (V1):
//   • CB source is EXCLUSIVE: if cb_source_path is non-empty → CB = full I1
//     read; else → CB = first cb_len bases of the decoded R1 read.
//   • Within a CB group, original read order is preserved (stable sort).
//   • All reads are emitted as forward ('d') orientation – no RC.
//   • Long-read mode (long_flag) is not supported; caller must skip this path.

#ifndef SPRING_BARCODE_SORT_H_
#define SPRING_BARCODE_SORT_H_

#include <string>

namespace spring {

struct compression_params;

// Performs barcode-first read ordering.
//
// temp_dir       — working directory containing input_clean_1.dna (and
//                  input_clean_2.dna when paired_end is true).
// cp             — compression parameters (assay, cb_len, num_thr, …).
// cb_source_path — when non-empty, cellular barcodes are extracted from this
//                  raw FASTQ (plain or .gz).  When empty, the first cb_len
//                  bases of each decoded R1 read are used instead.
void barcode_sort(const std::string &temp_dir, compression_params &cp,
                  const std::string &cb_source_path = std::string());

} // namespace spring

#endif // SPRING_BARCODE_SORT_H_
