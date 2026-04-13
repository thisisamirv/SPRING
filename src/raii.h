// Small RAII helpers for OpenMP locks and mmap-backed file mappings.
#ifndef SPRING_RAII_H_
#define SPRING_RAII_H_

#include <omp.h>
#include <utility>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#endif

#include <stdexcept>
#include <string>

namespace spring {

class OmpLock {
public:
  OmpLock() { omp_init_lock(&lock_); }
  ~OmpLock() { omp_destroy_lock(&lock_); }
  OmpLock(const OmpLock &) = delete;
  OmpLock &operator=(const OmpLock &) = delete;
  OmpLock(OmpLock &&) = delete;
  OmpLock &operator=(OmpLock &&) = delete;
  omp_lock_t *get() noexcept { return &lock_; }
  const omp_lock_t *get() const noexcept { return &lock_; }

private:
  omp_lock_t lock_;
};

class OmpLockGuard {
public:
  explicit OmpLockGuard(OmpLock &lock) : lock_ptr_(lock.get()) { omp_set_lock(lock_ptr_); }
  explicit OmpLockGuard(omp_lock_t *lock_ptr) : lock_ptr_(lock_ptr) { omp_set_lock(lock_ptr_); }
  ~OmpLockGuard() { omp_unset_lock(lock_ptr_); }
  OmpLockGuard(const OmpLockGuard &) = delete;
  OmpLockGuard &operator=(const OmpLockGuard &) = delete;

private:
  omp_lock_t *lock_ptr_;
};

#ifndef _WIN32
// Simple RAII wrapper for an mmap() region. The mapping is created from a
// file path and is automatically unmapped on destruction. The underlying file
// descriptor is opened and closed internally.
class MmapView {
public:
  MmapView() : mapping_(MAP_FAILED), size_(0) {}
  MmapView(const MmapView &) = delete;
  MmapView &operator=(const MmapView &) = delete;
  MmapView(MmapView &&other) noexcept : mapping_(other.mapping_), size_(other.size_) {
    other.mapping_ = MAP_FAILED;
    other.size_ = 0;
  }
  MmapView &operator=(MmapView &&other) noexcept {
    if (this != &other) {
      unmap();
      mapping_ = other.mapping_;
      size_ = other.size_;
      other.mapping_ = MAP_FAILED;
      other.size_ = 0;
    }
    return *this;
  }

  // Map the given file path of `size` bytes for read-only access. Throws
  // std::runtime_error on failure with errno-based message.
  void mapFromPath(const std::string &path, size_t size) {
    if (size == 0) {
      mapping_ = MAP_FAILED;
      size_ = 0;
      return;
    }
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::runtime_error("Error opening file: " + path + ": " +
                               std::string(std::strerror(errno)));
    }
    void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
      int saved = errno;
      close(fd);
      throw std::runtime_error("Error mapping file: " + path + ": " +
                               std::string(std::strerror(saved)));
    }
    // Close the fd early; the mapping remains valid.
    close(fd);
    mapping_ = mapped;
    size_ = size;
  }

  const char *data() const noexcept { return (mapping_ == MAP_FAILED) ? nullptr : static_cast<const char *>(mapping_); }
  size_t size() const noexcept { return size_; }

  void unmap() noexcept {
    if (mapping_ != MAP_FAILED) {
      munmap(mapping_, size_);
      mapping_ = MAP_FAILED;
      size_ = 0;
    }
  }

  ~MmapView() noexcept { unmap(); }

private:
  void *mapping_;
  size_t size_;
};
#endif // _WIN32

} // namespace spring

#endif // SPRING_RAII_H_
