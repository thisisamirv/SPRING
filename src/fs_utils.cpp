#include "fs_utils.h"
#include <string>
// Filesystem helpers: path manipulation, temporary-directory utilities, and
// cross-platform file operations used throughout the project.
#include "progress.h"
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace spring {

namespace {

void validate_archive_entry_name(const std::string &entry_name) {
  if (entry_name.empty()) {
    throw std::runtime_error("Archive contains an entry with an empty path.");
  }

  const std::filesystem::path entry_path(entry_name);
  if (entry_path.is_absolute() || entry_path.has_root_name() ||
      entry_path.has_root_directory()) {
    throw std::runtime_error("Archive entry path must be relative: " +
                             entry_path.generic_string());
  }

  for (const auto &part : entry_path) {
    if (part == "..") {
      throw std::runtime_error("Archive entry path escapes root: " +
                               entry_path.generic_string());
    }
  }
}

void write_archive_memory_entry(struct archive *archive_writer,
                                const std::string &entry_path,
                                const std::string &contents) {
  validate_archive_entry_name(entry_path);

  struct archive_entry *entry = archive_entry_new();
  if (entry == nullptr) {
    throw std::runtime_error("Failed to allocate archive entry for: " +
                             entry_path);
  }

  archive_entry_set_pathname(entry, entry_path.c_str());
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  if (archive_write_header(archive_writer, entry) != ARCHIVE_OK) {
    const char *message = archive_error_string(archive_writer);
    archive_entry_free(entry);
    throw std::runtime_error(
        "Failed to write archive header for '" + entry_path + "'" +
        (message ? ": " + std::string(message) : std::string()));
  }

  if (!contents.empty()) {
    const la_ssize_t written =
        archive_write_data(archive_writer, contents.data(), contents.size());
    if (written < 0 || written != static_cast<la_ssize_t>(contents.size())) {
      const char *message = archive_error_string(archive_writer);
      archive_entry_free(entry);
      throw std::runtime_error(
          "Failed to write archive data for '" + entry_path + "'" +
          (message ? ": " + std::string(message) : std::string()));
    }
  }

  archive_entry_free(entry);
}

bool path_is_within_directory(const std::filesystem::path &root,
                              const std::filesystem::path &candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  while (root_it != root.end() && candidate_it != candidate.end()) {
    if (*root_it != *candidate_it) {
      return false;
    }
    ++root_it;
    ++candidate_it;
  }
  return root_it == root.end();
}

std::filesystem::path
validated_archive_entry_destination(const std::filesystem::path &target_root,
                                    const char *entry_name) {
  if (entry_name == nullptr || entry_name[0] == '\0') {
    throw std::runtime_error("Archive contains an entry with an empty path.");
  }

  const std::filesystem::path entry_path(entry_name);
  if (entry_path.is_absolute() || entry_path.has_root_name() ||
      entry_path.has_root_directory()) {
    throw std::runtime_error("Archive contains an absolute extraction path: " +
                             entry_path.generic_string());
  }

  const std::filesystem::path destination =
      (target_root / entry_path).lexically_normal();
  if (!path_is_within_directory(target_root, destination)) {
    throw std::runtime_error(
        "Archive entry escapes the extraction directory: " +
        entry_path.generic_string());
  }

  return destination;
}

} // namespace

void copy_stream_buffered(std::istream &input_stream,
                          std::ostream &output_stream, size_t buffer_size) {
  std::vector<char> buffer(buffer_size);
  while (input_stream.read(buffer.data(),
                           static_cast<std::streamsize>(buffer.size()))) {
    output_stream.write(buffer.data(), input_stream.gcount());
  }
  if (input_stream.gcount() > 0) {
    output_stream.write(buffer.data(), input_stream.gcount());
  }
  output_stream.clear();
}

size_t get_directory_size(const std::string &temp_dir) {
  namespace fs = std::filesystem;
  size_t size = 0;
  std::error_code ec;
  fs::path p{temp_dir};

  if (p.has_relative_path() && !p.has_filename()) {
    p = p.parent_path();
  }

  if (!fs::exists(p, ec))
    return 0;

  for (const auto &entry : fs::recursive_directory_iterator(p, ec)) {
    if (ec)
      break;
    std::error_code size_ec;
    if (fs::is_regular_file(entry.path(), size_ec)) {
      size += fs::file_size(entry.path(), size_ec);
    }
  }
  return size;
}

std::string shell_quote(const std::string &value) {
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::string shell_path(const std::string &value) {
  return std::filesystem::path(value).generic_string();
}

bool safe_remove_file(const std::string &path) noexcept {
  if (path.empty())
    return true;
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    Logger::log_warning("Failed to remove file: " + path + ": " + ec.message());
    return false;
  }
  return true;
}

bool safe_rename_file(const std::string &old_path,
                      const std::string &new_path) noexcept {
  if (old_path.empty() || new_path.empty()) {
    Logger::log_warning("safe_rename_file called with empty path");
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(old_path, new_path, ec);
  if (!ec)
    return true;

  Logger::log_warning("Failed to rename file: " + old_path + " -> " + new_path +
                      ": " + ec.message());
  return false;
}

void create_tar_archive(const std::string &archive_path,
                        const std::string &source_dir) {
  struct archive *a;
  struct stat st;
  constexpr size_t kArchiveBufferSize = 4ULL * 1024 * 1024;
  std::vector<char> buffer(kArchiveBufferSize);
  uint64_t archived_file_count = 0;
  uint64_t archived_total_bytes = 0;

  SPRING_LOG_DEBUG("create_tar_archive start: source_dir=" + source_dir +
                   ", archive_path=" + archive_path);

  a = archive_write_new();
  archive_write_set_format_pax_restricted(a);
  auto close_archive = [&]() noexcept {
    if (a != nullptr) {
      archive_write_close(a);
      archive_write_free(a);
      a = nullptr;
    }
  };

  auto close_fd = [](int fd) noexcept {
#ifdef _WIN32
    if (fd >= 0)
      _close(fd);
#else
    if (fd >= 0)
      close(fd);
#endif
  };

  auto archive_error = [a](const std::string &prefix) {
    const char *message = archive_error_string(a);
    return std::runtime_error(
        prefix + (message ? ": " + std::string(message) : std::string()));
  };

  try {
    if (archive_write_open_filename(a, archive_path.c_str()) != ARCHIVE_OK) {
      throw archive_error("Failed to open archive for writing");
    }

    std::filesystem::path root(source_dir);
    for (const auto &dir_entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (!dir_entry.is_regular_file())
        continue;

      const std::string full_path = dir_entry.path().string();
      const std::string rel_path =
          std::filesystem::relative(dir_entry.path(), root).string();

      if (stat(full_path.c_str(), &st) != 0) {
        throw std::runtime_error("Failed to stat archive input '" + full_path +
                                 "': " + std::strerror(errno));
      }

      struct archive_entry *entry = archive_entry_new();
      if (entry == nullptr)
        throw std::runtime_error("Failed to allocate archive entry for: " +
                                 rel_path);
      archive_entry_set_pathname(entry, rel_path.c_str());
      archive_entry_set_size(entry, st.st_size);
      archive_entry_set_filetype(entry, AE_IFREG);
      archive_entry_set_perm(entry, 0644);
      if (archive_write_header(a, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        throw archive_error("Failed to write archive header for '" + rel_path +
                            "'");
      }

      int open_flags = O_RDONLY;
#if defined(_WIN32) && defined(O_BINARY) && (O_BINARY != 0)
      open_flags |= O_BINARY;
#endif
#if defined(O_CLOEXEC) && (O_CLOEXEC != 0)
      open_flags |= O_CLOEXEC;
#endif
      int fd = -1;
#ifdef _WIN32
      if (_sopen_s(&fd, full_path.c_str(), open_flags, _SH_DENYNO, _S_IREAD) !=
          0) {
        fd = -1;
      }
#else
      fd = open(full_path.c_str(), open_flags);
#endif
      if (fd < 0) {
        archive_entry_free(entry);
        throw std::runtime_error("Failed to open archive input '" + full_path +
                                 "': " + std::strerror(errno));
      }

      try {
#ifdef _WIN32
        int len =
            _read(fd, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
        ssize_t len = read(fd, buffer.data(), buffer.size());
#endif
        while (len > 0) {
          const la_ssize_t written = archive_write_data(a, buffer.data(), len);
          if (written < 0 || written != static_cast<la_ssize_t>(len)) {
            throw archive_error("Failed to write archive data for '" +
                                rel_path + "'");
          }
#ifdef _WIN32
          len = _read(fd, buffer.data(),
                      static_cast<unsigned int>(buffer.size()));
#else
          len = read(fd, buffer.data(), buffer.size());
#endif
        }
        if (len < 0) {
          throw std::runtime_error("Failed reading archive input '" +
                                   full_path + "': " + std::strerror(errno));
        }
      } catch (...) {
        close_fd(fd);
        archive_entry_free(entry);
        throw;
      }

      close_fd(fd);
      archive_entry_free(entry);
      archived_file_count++;
      archived_total_bytes += static_cast<uint64_t>(st.st_size);
    }

    if (archive_write_close(a) != ARCHIVE_OK) {
      throw archive_error("Failed to finalize archive");
    }
    archive_write_free(a);
    a = nullptr;
  } catch (...) {
    close_archive();
    throw;
  }

  SPRING_LOG_DEBUG(
      "create_tar_archive complete: files=" +
      std::to_string(archived_file_count) +
      ", total_input_bytes=" + std::to_string(archived_total_bytes));
}

void create_tar_archive_from_sources(
    const std::string &archive_path,
    const std::vector<tar_archive_source> &sources) {
  struct archive *archive_writer = archive_write_new();
  uint64_t archived_file_count = 0;
  uint64_t archived_total_bytes = 0;

  auto close_archive = [&]() noexcept {
    if (archive_writer != nullptr) {
      archive_write_close(archive_writer);
      archive_write_free(archive_writer);
      archive_writer = nullptr;
    }
  };

  try {
    archive_write_set_format_pax_restricted(archive_writer);
    if (archive_write_open_filename(archive_writer, archive_path.c_str()) !=
        ARCHIVE_OK) {
      const char *message = archive_error_string(archive_writer);
      throw std::runtime_error(
          "Failed to open archive for writing" +
          (message ? ": " + std::string(message) : std::string()));
    }

    for (const tar_archive_source &source : sources) {
      if (source.from_memory) {
        write_archive_memory_entry(archive_writer, source.archive_path,
                                   source.contents);
        archived_file_count++;
        archived_total_bytes += static_cast<uint64_t>(source.contents.size());
        continue;
      }

      std::ifstream input(source.disk_path, std::ios::binary);
      if (!input.is_open()) {
        throw std::runtime_error("Failed to open archive input '" +
                                 source.disk_path + "'.");
      }

      std::ostringstream content;
      content << input.rdbuf();
      if (!input.good() && !input.eof()) {
        throw std::runtime_error("Failed reading archive input '" +
                                 source.disk_path + "'.");
      }

      const std::string bytes = content.str();
      write_archive_memory_entry(archive_writer, source.archive_path, bytes);
      archived_file_count++;
      archived_total_bytes += static_cast<uint64_t>(bytes.size());
    }

    if (archive_write_close(archive_writer) != ARCHIVE_OK) {
      const char *message = archive_error_string(archive_writer);
      throw std::runtime_error(
          "Failed to finalize archive" +
          (message ? ": " + std::string(message) : std::string()));
    }
    archive_write_free(archive_writer);
    archive_writer = nullptr;
  } catch (...) {
    close_archive();
    throw;
  }

  SPRING_LOG_DEBUG(
      "create_tar_archive_from_sources complete: files=" +
      std::to_string(archived_file_count) +
      ", total_input_bytes=" + std::to_string(archived_total_bytes));
}

void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir) {
  struct archive *a;
  struct archive *ext;
  struct archive_entry *entry;
  int flags;
  int r;
  uint64_t extracted_entry_count = 0;
  uint64_t extracted_data_bytes = 0;

  SPRING_LOG_DEBUG("extract_tar_archive start: archive_path=" + archive_path +
                   ", target_dir=" + target_dir);

  flags = ARCHIVE_EXTRACT_PERM;
  flags |= ARCHIVE_EXTRACT_SECURE_NODOTDOT;
  flags |= ARCHIVE_EXTRACT_SECURE_SYMLINKS;

  a = archive_read_new();
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_none(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_empty(a);

  ext = archive_write_disk_new();
  archive_write_disk_set_options(ext, flags);

  auto close_archives = [&]() noexcept {
    if (a != nullptr) {
      archive_read_close(a);
      archive_read_free(a);
      a = nullptr;
    }
    if (ext != nullptr) {
      archive_write_close(ext);
      archive_write_free(ext);
      ext = nullptr;
    }
  };

  try {
    r = archive_read_open_filename(a, archive_path.c_str(), 10240);
    if (r != ARCHIVE_OK) {
      throw std::runtime_error("Failed to open archive for reading: " +
                               std::string(archive_error_string(a)));
    }

    std::filesystem::create_directories(target_dir);
    const std::filesystem::path target_root =
        std::filesystem::weakly_canonical(std::filesystem::path(target_dir));

    for (;;) {
      r = archive_read_next_header(a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r < ARCHIVE_WARN) {
        throw std::runtime_error("Error reading archive header: " +
                                 std::string(archive_error_string(a)));
      }

      const std::filesystem::path dest_path =
          validated_archive_entry_destination(target_root,
                                              archive_entry_pathname(entry));
      archive_entry_set_pathname(entry, dest_path.string().c_str());

      r = archive_write_header(ext, entry);
      extracted_entry_count++;
      if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
        const void *buff;
        size_t size;
        la_int64_t offset;
        while (true) {
          r = archive_read_data_block(a, &buff, &size, &offset);
          if (r == ARCHIVE_EOF)
            break;
          if (r < ARCHIVE_OK)
            throw std::runtime_error("Error reading archive data: " +
                                     std::string(archive_error_string(a)));
          r = archive_write_data_block(ext, buff, size, offset);
          extracted_data_bytes += static_cast<uint64_t>(size);
          if (r < ARCHIVE_OK)
            throw std::runtime_error("Error writing disk data: " +
                                     std::string(archive_error_string(ext)));
        }
      }
      r = archive_write_finish_entry(ext);
      if (r < ARCHIVE_OK)
        throw std::runtime_error("Error finishing disk entry: " +
                                 std::string(archive_error_string(ext)));
    }

    close_archives();
  } catch (...) {
    close_archives();
    throw;
  }
  SPRING_LOG_DEBUG("extract_tar_archive complete: entries=" +
                   std::to_string(extracted_entry_count) +
                   ", extracted_bytes=" + std::to_string(extracted_data_bytes));
}

void extract_tar_archive_from_memory(const std::string &archive_contents,
                                     const std::string &target_dir) {
  struct archive *a;
  struct archive *ext;
  struct archive_entry *entry;
  int flags;
  int r;
  uint64_t extracted_entry_count = 0;
  uint64_t extracted_data_bytes = 0;

  SPRING_LOG_DEBUG(
      "extract_tar_archive_from_memory start: target_dir=" + target_dir +
      ", archive_bytes=" + std::to_string(archive_contents.size()));

  flags = ARCHIVE_EXTRACT_PERM;
  flags |= ARCHIVE_EXTRACT_SECURE_NODOTDOT;
  flags |= ARCHIVE_EXTRACT_SECURE_SYMLINKS;

  a = archive_read_new();
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_none(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_empty(a);

  ext = archive_write_disk_new();
  archive_write_disk_set_options(ext, flags);

  auto close_archives = [&]() noexcept {
    if (a != nullptr) {
      archive_read_close(a);
      archive_read_free(a);
      a = nullptr;
    }
    if (ext != nullptr) {
      archive_write_close(ext);
      archive_write_free(ext);
      ext = nullptr;
    }
  };

  try {
    r = archive_read_open_memory(a, archive_contents.data(),
                                 archive_contents.size());
    if (r != ARCHIVE_OK) {
      throw std::runtime_error(
          "Failed to open in-memory archive for reading: " +
          std::string(archive_error_string(a)));
    }

    std::filesystem::create_directories(target_dir);
    const std::filesystem::path target_root =
        std::filesystem::weakly_canonical(std::filesystem::path(target_dir));

    for (;;) {
      r = archive_read_next_header(a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r < ARCHIVE_WARN) {
        throw std::runtime_error("Error reading archive header: " +
                                 std::string(archive_error_string(a)));
      }

      const std::filesystem::path dest_path =
          validated_archive_entry_destination(target_root,
                                              archive_entry_pathname(entry));
      archive_entry_set_pathname(entry, dest_path.string().c_str());

      r = archive_write_header(ext, entry);
      extracted_entry_count++;
      if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
        const void *buff;
        size_t size;
        la_int64_t offset;
        while (true) {
          r = archive_read_data_block(a, &buff, &size, &offset);
          if (r == ARCHIVE_EOF)
            break;
          if (r < ARCHIVE_OK)
            throw std::runtime_error("Error reading archive data: " +
                                     std::string(archive_error_string(a)));
          r = archive_write_data_block(ext, buff, size, offset);
          extracted_data_bytes += static_cast<uint64_t>(size);
          if (r < ARCHIVE_OK)
            throw std::runtime_error("Error writing disk data: " +
                                     std::string(archive_error_string(ext)));
        }
      }
      r = archive_write_finish_entry(ext);
      if (r < ARCHIVE_OK)
        throw std::runtime_error("Error finishing disk entry: " +
                                 std::string(archive_error_string(ext)));
    }

    close_archives();
  } catch (...) {
    close_archives();
    throw;
  }
  SPRING_LOG_DEBUG("extract_tar_archive_from_memory complete: entries=" +
                   std::to_string(extracted_entry_count) +
                   ", extracted_bytes=" + std::to_string(extracted_data_bytes));
}

std::unordered_map<std::string, std::string>
read_files_from_tar_bytes(const std::string &archive_contents,
                          const std::vector<std::string> &target_filenames) {
  struct archive *a;
  struct archive_entry *entry;
  int r;
  std::unordered_map<std::string, std::string> contents;

  a = archive_read_new();
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_xz(a);
  archive_read_support_filter_zstd(a);
  archive_read_support_filter_none(a);
  archive_read_support_format_tar(a);
  archive_read_support_format_empty(a);

  r = archive_read_open_memory(a, archive_contents.data(),
                               archive_contents.size());
  if (r != ARCHIVE_OK) {
    archive_read_free(a);
    return contents;
  }

  size_t found_count = 0;
  for (;;) {
    r = archive_read_next_header(a, &entry);
    if (r == ARCHIVE_EOF)
      break;
    if (r < ARCHIVE_WARN)
      break;

    std::string current_name = archive_entry_pathname(entry);
    bool is_target = false;
    for (const auto &tf : target_filenames) {
      if (current_name == tf) {
        is_target = true;
        break;
      }
    }

    if (is_target) {
      const void *buff;
      size_t size;
      la_int64_t offset;
      std::string content;
      while (true) {
        r = archive_read_data_block(a, &buff, &size, &offset);
        if (r == ARCHIVE_EOF)
          break;
        if (r < ARCHIVE_OK)
          break;
        content.append(static_cast<const char *>(buff), size);
      }
      contents[current_name] = content;
      found_count++;
      if (found_count == target_filenames.size()) {
        break;
      }
    }
  }

  archive_read_close(a);
  archive_read_free(a);
  return contents;
}

std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
                           const std::vector<std::string> &target_filenames) {
  std::ifstream input(archive_path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    return {};
  }

  return read_files_from_tar_bytes(contents.str(), target_filenames);
}

} // namespace spring
