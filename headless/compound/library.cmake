# SPDX-License-Identifier: GPL-3.0-or-later
# Target-only definitions; also safe inside ProsperoEden's DEFER callback.
include_guard(GLOBAL)
set(PROSPERO_COMPOUND_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(COMPOUND_VENDOR "${PROSPERO_COMPOUND_SOURCE_DIR}/../../third_party/compound_portable")
add_library(compound_portable STATIC
    "${PROSPERO_COMPOUND_SOURCE_DIR}/compound_ps5.c"
    "${COMPOUND_VENDOR}/src/core/cpu/interpreter.c"
    "${COMPOUND_VENDOR}/src/core/memory/guest_memory.c"
    "${COMPOUND_VENDOR}/src/core/guest_state.c"
    "${COMPOUND_VENDOR}/src/core/log.c"
    "${COMPOUND_VENDOR}/src/core/errors.c")
add_library(prospero-compound ALIAS compound_portable)
target_include_directories(compound_portable
    PUBLIC "${PROSPERO_COMPOUND_SOURCE_DIR}"
    PRIVATE "${COMPOUND_VENDOR}/src/core")
target_compile_features(compound_portable PRIVATE c_std_11)
set_target_properties(compound_portable PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
if(PS5_NATIVE)
    target_compile_options(compound_portable PRIVATE -femulated-tls)
endif()
