# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)
set(ZSTD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include(ZstdVersion)
project(zstd
    VERSION "${ZSTD_FULL_VERSION}"
    LANGUAGES C
    HOMEPAGE_URL "${zstd_HOMEPAGE_URL}"
    DESCRIPTION "${zstd_DESCRIPTION}"
)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()
include(GNUInstallDirs)
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "CMAKE_INSTALL_LIBDIR: ${CMAKE_INSTALL_LIBDIR}")
include(ZstdOptions)
include(AddZstdCompilationFlags)
ADD_ZSTD_COMPILATION_FLAGS(ON OFF ON)
include(ZstdDependencies)
include(ZstdBuild)
