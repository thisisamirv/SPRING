

#include <fcntl.h>

#include "qvz/lines.h"

namespace spring {
namespace qvz {

void free_blocks(struct quality_file_t *info) { free(info->blocks); }

} // namespace qvz
} // namespace spring
