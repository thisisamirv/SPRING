#ifndef SPRING_QVZ_UTIL_H_
#define SPRING_QVZ_UTIL_H_
/**
 * Utility functions to help do stuff and manage cross-platform issues
 */

#define _CRT_SECURE_NO_WARNINGS

#include <limits>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#define _stat stat
#define _alloca alloca
#endif

#ifndef restrict
#define restrict __restrict__
#endif

namespace spring {
namespace qvz {

// Explicit usage to satisfy linters
using limits_double = std::numeric_limits<double>;

// ceiling(log2()) function used in bit calculations
int cb_log2(int x);

// Missing log2 function
#ifndef LINUX
#define log2(x) (log(x) / log(2.0))
#endif

// Missing math symbols
#ifndef INFINITY
#define INFINITY limits_double::infinity()
#endif
#ifndef NAN
#define NAN limits_double::quiet_NaN()
#endif
} // namespace qvz
} // namespace spring

#endif
