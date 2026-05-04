// Declares low-level byte-pointer helpers used by binary I/O and archive
// serialization code throughout the project.

#ifndef SPRING_CORE_UTILS_H_
#define SPRING_CORE_UTILS_H_

namespace spring {

template <typename T> inline char *byte_ptr(T *value) {
  return reinterpret_cast<char *>(value);
}

template <typename T> inline const char *byte_ptr(const T *value) {
  return reinterpret_cast<const char *>(value);
}

} // namespace spring

#endif // SPRING_CORE_UTILS_H_
