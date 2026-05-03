#ifndef SPRING_QVZ_UTIL_H_
#define SPRING_QVZ_UTIL_H_

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <limits>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#define _stat stat
#define _alloca alloca
#endif

#ifndef restrict
#if defined(_MSC_VER)
#define restrict __restrict
#else
#define restrict __restrict__
#endif
#endif

namespace spring {
namespace qvz {

using limits_double = std::numeric_limits<double>;

int cb_log2(int x);

#ifndef LINUX
#define log2(x) (log(x) / log(2.0))
#endif

#ifndef INFINITY
#define INFINITY limits_double::infinity()
#endif
#ifndef NAN
#define NAN limits_double::quiet_NaN()
#endif
} // namespace qvz
} // namespace spring

#endif
