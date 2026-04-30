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

  r = archive_read_open_filename(a, archive_path.c_str(), 10240);
  if (r != ARCHIVE_OK) {
    throw std::runtime_error("Failed to open archive for reading: " +
                             std::string(archive_error_string(a)));
  }

  std::filesystem::create_directories(target_dir);

  for (;;) {
    r = archive_read_next_header(a, &entry);
    if (r == ARCHIVE_EOF)
      break;
    if (r < ARCHIVE_WARN) {
      throw std::runtime_error("Error reading archive header: " +
                               std::string(archive_error_string(a)));
    }

    std::filesystem::path dest_path =
        std::filesystem::path(target_dir) / archive_entry_pathname(entry);
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

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(ext);
  archive_write_free(ext);
  SPRING_LOG_DEBUG("extract_tar_archive complete: entries=" +
                   std::to_string(extracted_entry_count) +
                   ", extracted_bytes=" + std::to_string(extracted_data_bytes));
}
std::unordered_map<std::string, std::string>
read_files_from_tar_memory(const std::string &archive_path,
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

  r = archive_read_open_filename(a, archive_path.c_str(), 10240);
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

} // namespace spring
