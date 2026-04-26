# - Find pcre2posix
# Find the native PCRE2-8 and PCRE2-POSIX include and libraries
#
#  PCRE2_INCLUDE_DIR    - where to find pcre2posix.h, etc.
#  PCRE2POSIX_LIBRARIES - List of libraries when using libpcre2-posix.
#  PCRE2_LIBRARIES      - List of libraries when using libpcre2-8.
#  PCRE2POSIX_FOUND     - True if libpcre2-posix found.
#  PCRE2_FOUND          - True if libpcre2-8 found.

if(PCRE2_INCLUDE_DIR)
    # Already in cache, be silent
    set(PCRE2_FIND_QUIETLY TRUE)
endif(PCRE2_INCLUDE_DIR)

find_path(PCRE2_INCLUDE_DIR pcre2posix.h)
find_library(PCRE2POSIX_LIBRARY NAMES pcre2-posix libpcre2-posix pcre2-posix-static)
find_library(PCRE2_LIBRARY NAMES pcre2-8 libpcre2-8 pcre2-8-static)

# handle the QUIETLY and REQUIRED arguments and set PCRE2POSIX_FOUND to TRUE if 
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PCRE2POSIX DEFAULT_MSG PCRE2POSIX_LIBRARY PCRE2_INCLUDE_DIR)
find_package_handle_standard_args(PCRE2 DEFAULT_MSG PCRE2_LIBRARY)

if(PCRE2POSIX_FOUND)
    set(PCRE2POSIX_LIBRARIES ${PCRE2POSIX_LIBRARY})
    set(HAVE_LIBPCRE2POSIX 1)
    set(HAVE_PCRE2POSIX_H 1)
endif(PCRE2POSIX_FOUND)

if(PCRE2_FOUND)
    set(PCRE2_LIBRARIES ${PCRE2_LIBRARY})
    set(HAVE_LIBPCRE2 1)
endif(PCRE2_FOUND)
