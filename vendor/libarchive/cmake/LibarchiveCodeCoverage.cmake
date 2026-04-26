#################################################################
# Adds a build target called "coverage" for code coverage.
#
# This compiles the code using special GCC flags, run the tests,
# and then generates a nice HTML output. This new "coverage" make
# target will only be available if you build using GCC in Debug
# mode. If any of the required programs (lcov and genhtml) were
# not found, a FATAL_ERROR message is printed.
#
# If not already done, this code will set ENABLE_TEST to ON.
#
# To build the code coverage and open it in your browser do this:
#
#    mkdir debug
#    cd debug
#    cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
#    make -j4
#    make coverage
#    xdg-open coverage/index.html
#################################################################

# Find programs we need
find_program(LCOV_EXECUTABLE lcov DOC "Full path to lcov executable")
find_program(GENHTML_EXECUTABLE genhtml DOC "Full path to genhtml executable")
mark_as_advanced(LCOV_EXECUTABLE GENHTML_EXECUTABLE)

# Check, compiler, build types and programs are available
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Coverage can only be built on GCC")
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Coverage can only be built in Debug mode")
elseif(NOT LCOV_EXECUTABLE)
    message(FATAL_ERROR "lcov executable not found")
elseif(NOT GENHTML_EXECUTABLE)
    message(FATAL_ERROR "genhtml executable not found")
endif()

# Enable testing if not already done
set(ENABLE_TEST ON)

#################################################################
# Set special compiler and linker flags for test coverage
#################################################################
# 0. Enable debug: -g
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
# 1. Disable optimizations: -O0
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
# 2. Enable all kind of warnings:
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -W")
# 3. Enable special coverage flag (HINT: --coverage is a synonym for -fprofile-arcs -ftest-coverage)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
#################################################################

add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E echo "Beginning test coverage. Output is written to coverage.log."
    COMMAND ${CMAKE_COMMAND} -E echo "COVERAGE-STEP-1/5: Reset all execution counts to zero"
    COMMAND ${LCOV_EXECUTABLE} --directory . --zerocounters > coverage.log 2>&1
    COMMAND ${CMAKE_COMMAND} -E echo "COVERAGE-STEP-2/5: Run testrunner"
    COMMAND ${CMAKE_CTEST_COMMAND} >> coverage.log 2>&1
    COMMAND ${CMAKE_COMMAND} -E echo "COVERAGE-STEP-3/5: Collect coverage data"
    COMMAND ${LCOV_EXECUTABLE} --capture --directory . --output-file "./coverage.info" >> coverage.log 2>&1
    COMMAND ${CMAKE_COMMAND} -E echo "COVERAGE-STEP-4/5: Generate HTML from coverage data"
    COMMAND ${GENHTML_EXECUTABLE} "coverage.info" --title="libarchive-${LIBARCHIVE_VERSION_STRING}" --show-details --legend --output-directory "./coverage" >> coverage.log 2>&1
    COMMAND ${CMAKE_COMMAND} -E echo "COVERAGE-STEP-5/5: Open test coverage HTML output in browser: xdg-open ./coverage/index.html"
    COMMENT "Runs testrunner and generates coverage output (formats: .info and .html)")

