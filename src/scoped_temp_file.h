// Lightweight RAII helper for temporary files.
// Ensures a file is removed on destruction (destructor is noexcept).

#ifndef SPRING_SCOPED_TEMP_FILE_H_
#define SPRING_SCOPED_TEMP_FILE_H_

#include <filesystem>
#include <system_error>
#include <utility>

namespace spring {

class ScopedTempFile {
public:
  ScopedTempFile() noexcept = default;

  explicit ScopedTempFile(std::filesystem::path p,
                          bool remove_on_dtor = true) noexcept
      : path_(std::move(p)), remove_on_dtor_(remove_on_dtor) {}

  ~ScopedTempFile() noexcept { cleanup(); }

  ScopedTempFile(ScopedTempFile &&other) noexcept
      : path_(std::move(other.path_)), remove_on_dtor_(other.remove_on_dtor_) {
    other.remove_on_dtor_ = false;
  }

  ScopedTempFile &operator=(ScopedTempFile &&other) noexcept {
    if (this != &other) {
      cleanup();
      path_ = std::move(other.path_);
      remove_on_dtor_ = other.remove_on_dtor_;
      other.remove_on_dtor_ = false;
    }
    return *this;
  }

  ScopedTempFile(const ScopedTempFile &) = delete;
  ScopedTempFile &operator=(const ScopedTempFile &) = delete;

  // Path being managed.
  const std::filesystem::path &path() const noexcept { return path_; }

  // Release ownership; destructor will no longer remove the file.
  std::filesystem::path release() noexcept {
    remove_on_dtor_ = false;
    return std::exchange(path_, {});
  }

  // Replace the managed path (previous path removed if owned).
  void reset(std::filesystem::path p, bool remove_on_dtor = true) noexcept {
    cleanup();
    path_ = std::move(p);
    remove_on_dtor_ = remove_on_dtor;
  }

  // Prevent automatic removal in destructor.
  void keep() noexcept { remove_on_dtor_ = false; }

  // Remove file immediately; reports errors via `ec`.
  bool remove_now(std::error_code &ec) noexcept {
    ec.clear();
    if (path_.empty())
      return true;
    bool removed = std::filesystem::remove(path_, ec);
    if (!ec && removed)
      path_.clear();
    return !ec;
  }

  bool exists() const noexcept {
    std::error_code ec;
    return !path_.empty() && std::filesystem::exists(path_, ec);
  }

private:
  void cleanup() noexcept {
    if (!remove_on_dtor_ || path_.empty())
      return;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    (void)ec;
  }

  std::filesystem::path path_;
  bool remove_on_dtor_ = true;
};

} // namespace spring

#endif // SPRING_SCOPED_TEMP_FILE_H_
