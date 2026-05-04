#ifndef SPRING_FS_UTILS_H_
#define SPRING_FS_UTILS_H_

#include <iosfwd>
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

size_t get_directory_size(const std::string &temp_dir);

bool safe_remove_file(const std::string &path) noexcept;
bool safe_rename_file(const std::string &old_path,
                      const std::string &new_path) noexcept;

void copy_stream_buffered(std::istream &input_stream,
                          std::ostream &output_stream,
                          size_t buffer_size = 4 * 1024 * 1024);

void create_tar_archive(const std::string &archive_path,
                        const std::string &source_dir);
void create_tar_archive_from_sources(
    const std::string &archive_path,
    const std::vector<tar_archive_source> &sources);
void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir);
void extract_tar_archive_from_memory(const std::string &archive_contents,
                                     const std::string &target_dir);

std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
                           const std::vector<std::string> &target_filenames);
std::unordered_map<std::string, std::string>
read_files_from_tar_bytes(const std::string &archive_contents,
                          const std::vector<std::string> &target_filenames);

} // namespace spring

#endif // SPRING_FS_UTILS_H_
