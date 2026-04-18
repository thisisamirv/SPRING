#ifndef SPRING_FS_UTILS_H_
#define SPRING_FS_UTILS_H_

#include <iosfwd>
#include <string>

namespace spring {

size_t get_directory_size(const std::string &temp_dir);

std::string shell_quote(const std::string &value);
std::string shell_path(const std::string &value);

bool safe_remove_file(const std::string &path) noexcept;
bool safe_rename_file(const std::string &old_path,
                      const std::string &new_path) noexcept;

void copy_stream_buffered(std::istream &input_stream,
                          std::ostream &output_stream,
                          size_t buffer_size = 4 * 1024 * 1024);

void create_tar_archive(const std::string &archive_path,
                        const std::string &source_dir);
void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir);

} // namespace spring

#endif // SPRING_FS_UTILS_H_
