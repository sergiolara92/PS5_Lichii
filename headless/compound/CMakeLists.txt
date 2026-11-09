cmake_minimum_required(VERSION 3.20)

set(COMPOUND_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ProsperoCompoundPortable VERSION 1.0.0 LANGUAGES C CXX)
    set(COMPOUND_STANDALONE ON)
endif()

# Build only the original portable CPU path. Do not configure Compound's full
# SDL/OpenGL shell or import host-specific Ballistic/LuaJIT binary libraries.
include("${CMAKE_CURRENT_LIST_DIR}/library.cmake")

option(COMPOUND_BUILD_TESTS "Build standalone portable Compound host checks"
       ${COMPOUND_STANDALONE})
if(COMPOUND_BUILD_TESTS)
    enable_testing()
    add_executable(compound_portable_tests tests/test_compound.c)
    target_link_libraries(compound_portable_tests PRIVATE compound_portable)
    target_include_directories(compound_portable_tests PRIVATE "${COMPOUND_VENDOR}/src/core")
    target_compile_features(compound_portable_tests PRIVATE c_std_11)
    set_target_properties(compound_portable_tests PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
    add_test(NAME compound.portable.real_a64 COMMAND compound_portable_tests)

    # Compile the actual C++ frontend consumer, covering the C linkage boundary
    # and both enabled/disabled builds without pulling Eden into the harness.
    add_executable(compound_frontend_tests tests/test_frontend.cpp)
    target_link_libraries(compound_frontend_tests PRIVATE compound_portable)
    target_compile_definitions(compound_frontend_tests PRIVATE PROSPERO_ENABLE_COMPOUND=1)
    target_compile_features(compound_frontend_tests PRIVATE cxx_std_17)
    add_test(NAME compound.frontend.enabled COMMAND compound_frontend_tests)

    add_executable(compound_frontend_disabled_tests tests/test_frontend.cpp)
    target_compile_features(compound_frontend_disabled_tests PRIVATE cxx_std_17)
    add_test(NAME compound.frontend.disabled COMMAND compound_frontend_disabled_tests)
endif()
