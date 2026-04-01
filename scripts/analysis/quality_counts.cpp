#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kReadLen = 147;
constexpr int kNumClusters = 1;
constexpr int kQualityValueCount = 42;

const std::vector<int> kClusterBoundaries = {};

size_t count1_index(const int cluster_num, const int position,
                    const int quality_value) {
  return (static_cast<size_t>(cluster_num) * kReadLen + position) *
             kQualityValueCount +
         quality_value;
}

size_t count2_index(const int cluster_num, const int position,
                    const int quality_value_1, const int quality_value_2) {
  return (((static_cast<size_t>(cluster_num) * (kReadLen - 1) + position) *
               kQualityValueCount +
           quality_value_1) *
              kQualityValueCount) +
         quality_value_2;
}

size_t count3_index(const int cluster_num, const int position,
                    const int quality_value_1, const int quality_value_2,
                    const int quality_value_3) {
  return ((((static_cast<size_t>(cluster_num) * (kReadLen - 2) + position) *
                kQualityValueCount +
            quality_value_1) *
               kQualityValueCount +
           quality_value_2) *
              kQualityValueCount) +
         quality_value_3;
}

int find_cluster(const double avg_qv) {
  int cluster_num = -1;
  for (int cluster_index = 0; cluster_index < kNumClusters - 1; ++cluster_index) {
    if (avg_qv < kClusterBoundaries[static_cast<size_t>(cluster_index)]) {
      cluster_num = cluster_index;
    }
  }

  if (cluster_num == -1) {
    cluster_num = kNumClusters - 1;
  }
  return cluster_num;
}

void write_counts(std::ofstream &f_out, const std::vector<uint64_t> &count1,
                  const std::vector<uint64_t> &count2,
                  const std::vector<uint64_t> &count3,
                  const std::vector<uint64_t> &num_reads) {
  for (int cluster_num = 0; cluster_num < kNumClusters; ++cluster_num) {
    f_out.write(reinterpret_cast<const char *>(
                    &num_reads[static_cast<size_t>(cluster_num)]),
                sizeof(uint64_t));

    for (int position = 0; position < kReadLen; ++position) {
      for (int quality_value = 0; quality_value < kQualityValueCount;
           ++quality_value) {
        const uint64_t value =
            count1[count1_index(cluster_num, position, quality_value)];
        f_out.write(reinterpret_cast<const char *>(&value), sizeof(uint64_t));
      }
    }

    for (int position = 0; position < kReadLen - 1; ++position) {
      for (int quality_value_1 = 0; quality_value_1 < kQualityValueCount;
           ++quality_value_1) {
        for (int quality_value_2 = 0; quality_value_2 < kQualityValueCount;
             ++quality_value_2) {
          const uint64_t value = count2[count2_index(
              cluster_num, position, quality_value_1, quality_value_2)];
          f_out.write(reinterpret_cast<const char *>(&value),
                      sizeof(uint64_t));
        }
      }
    }

    for (int position = 0; position < kReadLen - 2; ++position) {
      for (int quality_value_1 = 0; quality_value_1 < kQualityValueCount;
           ++quality_value_1) {
        for (int quality_value_2 = 0; quality_value_2 < kQualityValueCount;
             ++quality_value_2) {
          for (int quality_value_3 = 0; quality_value_3 < kQualityValueCount;
               ++quality_value_3) {
            const uint64_t value = count3[count3_index(
                cluster_num, position, quality_value_1, quality_value_2,
                quality_value_3)];
            f_out.write(reinterpret_cast<const char *>(&value),
                        sizeof(uint64_t));
          }
        }
      }
    }
  }
}

void compute_counts(const std::string &infile, const std::string &outfile) {
  std::string line;
  std::ifstream myfile(infile, std::ifstream::in);
  std::vector<uint64_t> count1(static_cast<size_t>(kNumClusters) * kReadLen *
                               kQualityValueCount);
  std::vector<uint64_t> count2(static_cast<size_t>(kNumClusters) *
                               (kReadLen - 1) * kQualityValueCount *
                               kQualityValueCount);
  std::vector<uint64_t> count3(static_cast<size_t>(kNumClusters) *
                               (kReadLen - 2) * kQualityValueCount *
                               kQualityValueCount * kQualityValueCount);
  std::vector<uint64_t> num_reads(static_cast<size_t>(kNumClusters));

  while (std::getline(myfile, line)) {
    int total_qv = 0;
    for (int position = 0; position < kReadLen; ++position) {
      total_qv += (static_cast<unsigned int>(line[position]) - 33);
    }
    const double avg_qv = static_cast<double>(total_qv) / kReadLen;
    const int cluster_num = find_cluster(avg_qv);

    ++num_reads[static_cast<size_t>(cluster_num)];
    for (int position = 0; position < kReadLen - 2; ++position) {
      const int quality_value_1 = static_cast<unsigned int>(line[position]) - 33;
      const int quality_value_2 =
          static_cast<unsigned int>(line[position + 1]) - 33;
      const int quality_value_3 =
          static_cast<unsigned int>(line[position + 2]) - 33;
      ++count1[count1_index(cluster_num, position, quality_value_1)];
      ++count2[count2_index(cluster_num, position, quality_value_1,
                            quality_value_2)];
      ++count3[count3_index(cluster_num, position, quality_value_1,
                            quality_value_2, quality_value_3)];
    }

    int position = kReadLen - 2;
    const int quality_value_1 = static_cast<unsigned int>(line[position]) - 33;
    const int quality_value_2 = static_cast<unsigned int>(line[position + 1]) - 33;
    ++count1[count1_index(cluster_num, position, quality_value_1)];
    ++count2[count2_index(cluster_num, position, quality_value_1,
                          quality_value_2)];

    position = kReadLen - 1;
    ++count1[count1_index(cluster_num, position,
                          static_cast<unsigned int>(line[position]) - 33)];
  }
  myfile.close();

  std::ofstream f_out(outfile, std::ios::binary);
  write_counts(f_out, count1, count2, count3, num_reads);
}

} // namespace

int main(int, char **argv) {
  compute_counts(argv[1], argv[2]);
  return 0;
}
