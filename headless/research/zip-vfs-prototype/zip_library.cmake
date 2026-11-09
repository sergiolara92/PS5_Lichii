# SPDX-License-Identifier: GPL-3.0-or-later
# Target-only include, safe when called from cmake_language(DEFER).
include_guard(GLOBAL)
get_filename_component(_zipvfs_source_root "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
if(NOT DEFINED EDEN_SOURCE_ROOT)
    get_filename_component(EDEN_SOURCE_ROOT "${_zipvfs_source_root}/../../analysis/eden-source" ABSOLUTE)
endif()
if(NOT DEFINED ZIPVFS_COMMON_VFS_INCLUDE_ROOT)
    # Explicit isolated dependency until the common Eden VFS error patch lands.
    get_filename_component(ZIPVFS_COMMON_VFS_INCLUDE_ROOT
        "${_zipvfs_source_root}/../direct-loader-prototype/eden-hook-src/src" ABSOLUTE)
endif()
if(NOT EXISTS "${ZIPVFS_COMMON_VFS_INCLUDE_ROOT}/core/file_sys/vfs/vfs_read_error.h")
    message(FATAL_ERROR "ZIP VFS needs the common FileSys::VfsReadError header; set ZIPVFS_COMMON_VFS_INCLUDE_ROOT")
endif()
# Research-native integration reuses the exact existing baseline miniz.
# Its full stdio configuration remains unchanged for save handling.
if(NOT DEFINED ZIPVFS_MINIZ_TARGET OR ZIPVFS_MINIZ_TARGET STREQUAL "")
    message(FATAL_ERROR "Set ZIPVFS_MINIZ_TARGET to the reviewed existing miniz target")
endif()
if(NOT TARGET "${ZIPVFS_MINIZ_TARGET}")
    message(FATAL_ERROR "Requested miniz target does not exist: ${ZIPVFS_MINIZ_TARGET}")
endif()
add_library(eden_zip_vfs STATIC "${_zipvfs_source_root}/zip_vfs.cpp")
target_include_directories(eden_zip_vfs PUBLIC "${_zipvfs_source_root}"
    "${ZIPVFS_COMMON_VFS_INCLUDE_ROOT}" "${EDEN_SOURCE_ROOT}/src")
target_compile_features(eden_zip_vfs PUBLIC cxx_std_20)
target_link_libraries(eden_zip_vfs PRIVATE "${ZIPVFS_MINIZ_TARGET}")
# Header aliases only: this does not change mz_* layouts or library ABI.
target_compile_definitions(eden_zip_vfs PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(NOT WIN32)
    target_compile_definitions(eden_zip_vfs PRIVATE _FILE_OFFSET_BITS=64)
endif()
