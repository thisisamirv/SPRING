#pragma once

#include <filesystem>
#include <string>

namespace spring {

void preview(const std::string &archive_path, bool audit_only,
             const std::filesystem::path &working_dir = ".");

} // namespace spring