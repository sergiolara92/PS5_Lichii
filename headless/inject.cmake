# SPDX-License-Identifier: GPL-3.0-or-later
# Add our frontend after the pinned project's real core targets exist.
set(EDEN_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}")
# EDEN_ANCHOR_SCAN=1 (environment, moving to a new base): a pinned anchor that is missing is a warning
# ("ANCHOR-MISS ...") instead of an error, so one configure lists every miss. Never for a build to keep.
if(DEFINED ENV{EDEN_ANCHOR_SCAN})
    function(message mode)
        if(mode STREQUAL "FATAL_ERROR" AND "${ARGV1}" MATCHES "^Pinned")
            _message(WARNING "ANCHOR-MISS ${ARGV1}")
        else()
            _message(${ARGV})
        endif()
    endfunction()
endif()
function(add_eden_headless)
    include("${EDEN_PORT_DIR}/CMakeLists.txt")
endfunction()
cmake_language(DEFER CALL add_eden_headless)
# One header is replaced for every target: common/sparse_large_vector.h, whose inline functions
# decide how Eden's large tables take their memory (headless/CMakeLists.txt writes the derived
# copy). This runs before any target exists, so the folder leads every include path: a source
# file that saw the original would mix two ways of committing the same table.
include_directories(BEFORE "${CMAKE_BINARY_DIR}/headless/sparse")
# The program names its sources in log lines and assertions. In a console build they are written
# relative to the trees they come from (src/..., build/..., headless/...), not with the folders of
# the machine that built it: a package must not carry those. Before any target exists, so it holds
# for every one. A PGO build (EDEN_PGO_GENERATE / EDEN_PGO_USE, headless/CMakeLists.txt) does the
# same on the PC: profile names of file-local functions carry these paths, so a profile trained on
# the PC build matches the console build.
if(PS5_NATIVE OR EDEN_PGO_GENERATE OR EDEN_PGO_USE)
    get_filename_component(EDEN_PORT_ROOT "${EDEN_PORT_DIR}/.." ABSOLUTE)
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=build/"
                        "-ffile-prefix-map=${EDEN_PORT_ROOT}/=")
endif()
# Host-only research profile (2026-10-29): the PS5 research route's PAuth-disabled JIT profile and
# remote region, selected at run time by the PROSPERO_RESEARCH_* variables, for desktop traces.
option(EDEN_HOST_RESEARCH_PROFILE "Host build: compile the env-selected native research profiles" OFF)
if(EDEN_HOST_RESEARCH_PROFILE AND NOT PS5_NATIVE)
    function(eden_host_research_profile)
        target_compile_definitions(core PRIVATE PROSPERO_NATIVE_STARTUP_RESEARCH=1 PROSPERO_CPU_TRACE=1)
    endfunction()
    cmake_language(DEFER CALL eden_host_research_profile)
endif()
