// Declares filesystem and tar-archive helpers used by compression,
// decompression, preview, and audit flows.

#ifndef SPRING_FS_UTILS_H_
#define SPRING_FS_UTILS_H_

#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct tar_archive_source {
  std::string archive_path;
  std::string disk_path;
  std::string contents;
  bool from_memory = false;
};

bool safe_remove_file(const std::string &path) noexcept;
void create_tar_archive_from_sources(
    const std::string &archive_path,
    const std::vector<tar_archive_source> &sources);
std::string create_tar_archive_from_sources_bytes(
    const std::vector<tar_archive_source> &sources);
void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir);

std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
                           const std::vector<std::string> &target_filenames);
std::unordered_map<std::string, std::string>
read_files_from_tar_bytes(const std::string &archive_contents,
                          const std::vector<std::string> &target_filenames);
std::unordered_map<std::string, std::string>
read_all_files_from_tar_memory(const std::string &archive_path);
std::unordered_map<std::string, std::string>
read_all_files_from_tar_bytes(const std::string &archive_contents);

} // namespace spring

#endif // SPRING_FS_UTILS_H_
