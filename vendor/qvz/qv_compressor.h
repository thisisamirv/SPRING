#ifndef SPRING_QVZ_QV_COMPRESSOR_H_
#define SPRING_QVZ_QV_COMPRESSOR_H_

#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

#define m_arith 22

#define OS_STREAM_BUF_LEN (4096 * 4096)

#define COMPRESSION 0
#define DECOMPRESSION 1

namespace spring {
namespace qvz {

void start_qv_quantization(struct quality_file_t *info);

}
} // namespace spring

#endif
