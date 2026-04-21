// Precompiled header for SPRING2
// Includes stable, frequently-used headers across translation units.
// This header is compiled once and cached by the compiler, speeding up
// incremental rebuilds significantly.

#ifndef SPRING_PCH_H_
#define SPRING_PCH_H_

// IWYU pragma: begin_exports

// Standard library: containers and strings (used in nearly all translation
// units)
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Concurrency
#include <atomic>
#include <mutex>
#include <thread>

// OpenMP (used across encoder, decoder, and parallel utilities)
#include <omp.h>

// IWYU pragma: end_exports

#endif // SPRING_PCH_H_
