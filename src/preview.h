// Declares archive preview entry points used by the CLI preview and audit
// mode dispatch.

#pragma once

#include <string>

namespace spring {

void preview(const std::string &archive_path, bool audit_only);

} // namespace spring