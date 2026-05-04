// Declares small parsing and string-conversion helpers used across command
// handling and record parsing code.

#ifndef SPRING_PARSE_UTILS_H_
#define SPRING_PARSE_UTILS_H_

#include <cstdint>
#include <string>

namespace spring {

void remove_CR_from_end(std::string &str);
bool has_suffix(const std::string &value, const std::string &suffix);

int parse_int_or_throw(const std::string &value, const char *error_message);
double parse_double_or_throw(const std::string &value,
                             const char *error_message);
uint64_t parse_uint64_or_throw(const std::string &value,
                               const char *error_message);

// ID pattern recognition.
uint8_t find_id_pattern(const std::string &id_1, const std::string &id_2);
bool check_id_pattern(const std::string &id_1, const std::string &id_2,
                      uint8_t paired_id_code);
void modify_id(std::string &id, const uint8_t paired_id_code);

} // namespace spring

#endif // SPRING_PARSE_UTILS_H_
