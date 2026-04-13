// Small RAII helpers for OpenMP locks and mmap-backed file mappings.
#ifndef SPRING_RAII_H_
#define SPRING_RAII_H_

#include <omp.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
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
  explicit OmpLockGuard(OmpLock &lock) : lock_ptr_(lock.get()) {
    omp_set_lock(lock_ptr_);
  }
  explicit OmpLockGuard(omp_lock_t *lock_ptr) : lock_ptr_(lock_ptr) {
    omp_set_lock(lock_ptr_);
  }
  ~OmpLockGuard() { omp_unset_lock(lock_ptr_); }
  OmpLockGuard(const OmpLockGuard &) = delete;
  OmpLockGuard &operator=(const OmpLockGuard &) = delete;

private:
  omp_lock_t *lock_ptr_;
};

// Simple RAII wrapper for an mmap() region. The mapping is created from a
// file path and is automatically unmapped on destruction.
class MmapView {
public:
  MmapView() : mapping_(nullptr), size_(0) {}
  MmapView(const MmapView &) = delete;
  MmapView &operator=(const MmapView &) = delete;
  MmapView(MmapView &&other) noexcept
      : mapping_(other.mapping_), size_(other.size_) {
    other.mapping_ = nullptr;
    other.size_ = 0;
  }
  MmapView &operator=(MmapView &&other) noexcept {
    if (this != &other) {
      unmap();
      mapping_ = other.mapping_;
      size_ = other.size_;
      other.mapping_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  // Map the given file path of `size` bytes for read-only access. Throws
  // std::runtime_error on failure.
  void mapFromPath(const std::string &path, size_t size) {
    if (size == 0) {
      mapping_ = nullptr;
      size_ = 0;
      return;
    }
#ifdef _WIN32
    HANDLE file_handle =
        CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Error opening file for mapping: " + path);
    }
    HANDLE map_handle =
        CreateFileMappingA(file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (map_handle == nullptr) {
      CloseHandle(file_handle);
      throw std::runtime_error("Error creating file mapping: " + path);
    }
    void *mapped = MapViewOfFile(map_handle, FILE_MAP_READ, 0, 0, size);
    if (mapped == nullptr) {
      CloseHandle(map_handle);
      CloseHandle(file_handle);
      throw std::runtime_error("Error mapping view of file: " + path);
    }
    CloseHandle(map_handle);
    CloseHandle(file_handle);
    mapping_ = mapped;
#else
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
    close(fd);
    mapping_ = mapped;
#endif
    size_ = size;
  }

  const char *data() const noexcept {
    return static_cast<const char *>(mapping_);
  }
  size_t size() const noexcept { return size_; }

  void unmap() noexcept {
    if (mapping_ != nullptr) {
#ifdef _WIN32
      UnmapViewOfFile(mapping_);
#else
      munmap(mapping_, size_);
#endif
      mapping_ = nullptr;
      size_ = 0;
    }
  }

  ~MmapView() noexcept { unmap(); }

private:
  void *mapping_;
  size_t size_;
};

} // namespace spring

#endif // SPRING_RAII_H_
