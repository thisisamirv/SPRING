#ifndef PTHASH_EXTERNAL_MM_FILE_MM_FILE_HPP
#define PTHASH_EXTERNAL_MM_FILE_MM_FILE_HPP

#ifndef _WIN32
#include <dirent.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#else
#include "pthash_windefs.h"
#endif

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mm {

namespace advice {
static const int normal = POSIX_MADV_NORMAL;
static const int random = POSIX_MADV_RANDOM;
static const int sequential = POSIX_MADV_SEQUENTIAL;
} // namespace advice

template <typename T> struct file {
  file() { init(); }

  ~file() {
    try {
      close();
    } catch (...) {
    }
  }

  file(file const &) = delete;            // non construction-copyable
  file &operator=(file const &) = delete; // non copyable

  file(file &&other) noexcept
      : m_fd(other.m_fd), m_size(other.m_size), m_data(other.m_data) {
    other.init();
  }

  file &operator=(file &&other) noexcept {
    if (this != &other) {
      try {
        close();
      } catch (...) {
      }
      m_fd = other.m_fd;
      m_size = other.m_size;
      m_data = other.m_data;
      other.init();
    }
    return *this;
  }

  [[nodiscard]] bool is_open() const { return m_fd != -1; }

  void close() {
    if (is_open()) {
      if (munmap((char *)m_data, m_size) == -1) {
        throw std::runtime_error("munmap failed when closing file");
      }
      ::close(m_fd);
      init();
    }
  }

  [[nodiscard]] size_t bytes() const { return m_size; }

  [[nodiscard]] size_t size() const { return m_size / sizeof(T); }

  [[nodiscard]] T *data() const { return m_data; }

  struct iterator {
    using iterator_category = std::random_access_iterator_tag;

    iterator(T *addr, size_t offset = 0) : m_ptr(addr + offset) {}

    T operator*() { return *m_ptr; }

    void operator++() { ++m_ptr; }

    iterator operator+(uint64_t jump) {
      iterator copy(m_ptr + jump);
      return copy;
    }

    iterator operator-(uint64_t jump) {
      iterator copy(m_ptr - jump);
      return copy;
    }

    bool operator==(iterator const &rhs) const { return m_ptr == rhs.m_ptr; }

    bool operator!=(iterator const &rhs) const { return !((*this) == rhs); }

  private:
    T *m_ptr;
  };

  iterator begin() const { return iterator(m_data); }

  iterator end() const { return iterator(m_data, size()); }

private:
  int m_fd;
  size_t m_size;
  T *m_data;

protected:
  void set_fd(int fd) { m_fd = fd; }
  [[nodiscard]] int fd() const { return m_fd; }
  void set_size(size_t s) { m_size = s; }
  void set_data(T *d) { m_data = d; }

  void init() {
    m_fd = -1;
    m_size = 0;
    m_data = nullptr;
  }

  void check_fd() {
    if (m_fd == -1)
      throw std::runtime_error("cannot open file");
  }
};

template <typename Pointer> Pointer mmap(int fd, size_t size, int prot) {
  static const size_t offset = 0;
  Pointer p =
      static_cast<Pointer>(::mmap(NULL, size, prot, MAP_SHARED, fd, offset));
  if (p == MAP_FAILED)
    throw std::runtime_error("mmap failed");
  return p;
}

template <typename T> struct file_source : public file<T const> {
  typedef file<T const> base;

  file_source() = default;

  file_source(std::string const &path, int adv = advice::normal) {
    open(path, adv);
  }

  void open(std::string const &path, int adv = advice::normal) {
    int fd;
#ifdef O_CLOEXEC
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#else
    fd = ::open(path.c_str(), O_RDONLY);
#endif
    base::set_fd(fd);
    base::check_fd();
    struct stat fs;
    if (fstat(base::fd(), &fs) == -1) {
      throw std::runtime_error("cannot stat file");
    }
    base::set_size(fs.st_size);
    base::set_data(mmap<T const *>(base::fd(), base::bytes(), PROT_READ));
    if (posix_madvise((void *)base::m_data, base::m_size, adv)) {
      throw std::runtime_error("madvise failed");
    }
  }
};

template <typename T> struct file_sink : public file<T> {
  typedef file<T> base;

  file_sink() = default;

  file_sink(std::string const &path) { open(path); }

  file_sink(std::string const &path, size_t n) { open(path, n); }

  void open(std::string const &path) {
    static const mode_t mode = 0600; // read/write
    int fd;
#ifdef O_CLOEXEC
    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC, mode);
#else
    fd = ::open(path.c_str(), O_RDWR, mode);
#endif
    base::set_fd(fd);
    base::check_fd();
    struct stat fs;
    if (fstat(base::fd(), &fs) == -1) {
      throw std::runtime_error("cannot stat file");
    }
    base::set_size(fs.st_size);
    base::set_data(
        mmap<T *>(base::fd(), base::bytes(), PROT_READ | PROT_WRITE));
  }

  void open(std::string const &path, size_t n) {
    static const mode_t mode = 0600; // read/write
    int fd;
#ifdef O_CLOEXEC
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
#else
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, mode);
#endif
    base::set_fd(fd);
    base::check_fd();
    base::set_size(n * sizeof(T));
    ftruncate(base::fd(), base::bytes()); // truncate the file at the new size
    base::set_data(
        mmap<T *>(base::fd(), base::bytes(), PROT_READ | PROT_WRITE));
  }
};

} // namespace mm

#endif // PTHASH_EXTERNAL_MM_FILE_MM_FILE_HPP