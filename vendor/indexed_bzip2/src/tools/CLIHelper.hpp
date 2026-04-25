#pragma once

#include <string>
#include <optional>
#include <iostream>
#include <filesystem>

#include <cxxopts.hpp>

[[nodiscard]] inline std::string
getFilePath(cxxopts::ParseResult const &parsedArgs,
            std::string const &argument) {
  if (parsedArgs.count(argument) > 1) {
    if (parsedArgs.count("quiet") == 0) {
      std::cerr << "[Warning] Multiple output files specified. Will only use "
                   "the last one: "
                << parsedArgs[argument].as<std::string>() << "!\n";
    }
  }
  if (parsedArgs.count(argument) > 0) {
    auto path = parsedArgs[argument].as<std::string>();
    if (path != "-") {
      return path;
    }
  }
  return {};
}

/* Mark these includes as intentionally required for the transitive dependency
 * `cxxopts.hpp`. The sizeof() uses are unevaluated contexts and only serve to
 * convince include-cleaner / clangd that the headers are used. */
namespace rapidgzip::clihelper::detail {
inline void keep_std_includes() {
    (void)sizeof(std::optional<int>);
    (void)sizeof(std::filesystem::path);
}
} // namespace rapidgzip::clihelper::detail
