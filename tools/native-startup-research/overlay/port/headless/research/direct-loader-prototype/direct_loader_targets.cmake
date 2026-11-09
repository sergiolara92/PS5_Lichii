# Target-only integration, safe to include from Prospero's deferred frontend.
# This file is deliberately not included by the active/native build.
include_guard(GLOBAL)
if(NOT TARGET core)
    message(FATAL_ERROR "Include direct_loader_targets.cmake inside the configured Eden project after core exists")
endif()
set(direct_loader_root "${CMAKE_CURRENT_LIST_DIR}")
option(PROSPERO_DIRECT_REQUIRE_EXPERIMENTAL_NPDM "Require the independently tested NPDM parser for actual-title mapping" ON)
if(PROSPERO_DIRECT_REQUIRE_EXPERIMENTAL_NPDM)
    file(SHA256 "${PROJECT_SOURCE_DIR}/src/core/file_sys/program_metadata.cpp" direct_npdm_source_sha)
    if(NOT direct_npdm_source_sha STREQUAL "53003a481e46c6651b14be87e21b47cf24dd0435b2cc979028981aa01810ea94")
        message(FATAL_ERROR "Actual-title load-only probe requires the separately reviewed pinned dual-layout NPDM source; this include does not apply it")
    endif()
endif()
add_library(prospero_direct_development_loader STATIC
    "${direct_loader_root}/development_input.cpp"
    "${direct_loader_root}/direct_development_loader.cpp")
target_include_directories(prospero_direct_development_loader PUBLIC "${direct_loader_root}"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
# Prospero shadows core headers with derived layouts. The research consumer must
# see the same private include ordering/definitions as core, just as the object
# check used core's real compile database flags. Raw upstream headers alone can
# silently give incompatible inline field access.
target_include_directories(prospero_direct_development_loader BEFORE PUBLIC
    "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
target_compile_definitions(prospero_direct_development_loader PUBLIC
    "$<TARGET_PROPERTY:core,COMPILE_DEFINITIONS>")
target_link_libraries(prospero_direct_development_loader PUBLIC core)
target_compile_features(prospero_direct_development_loader PRIVATE cxx_std_20)
target_compile_options(prospero_direct_development_loader PRIVATE -fno-rtti)

# Absent by default. Review/apply the isolated Eden factory/read-error patches
# and the generated frontend patch in a separate research checkout first.
option(PROSPERO_DIRECT_FRONTEND_ROUTE "Compile the explicit research-input.json route" OFF)
option(PROSPERO_DIRECT_ALLOW_EXECUTE "Allow unvalidated research guest execution (mapping remains the default)" OFF)
if(PROSPERO_DIRECT_FRONTEND_ROUTE)
    if(NOT TARGET eden-headless)
        message(FATAL_ERROR "The explicit frontend route requires target eden-headless")
    endif()
    foreach(required_file IN ITEMS
            "src/core/loader/development_factory.h"
            "src/core/loader/development_factory.cpp"
            "src/core/file_sys/vfs/vfs_read_error.h")
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${required_file}")
            message(FATAL_ERROR "Apply the reviewed research Eden patches first: missing ${required_file}")
        endif()
    endforeach()
    file(READ "${PROJECT_SOURCE_DIR}/src/core/loader/loader.cpp" direct_loader_dispatch_source)
    string(FIND "${direct_loader_dispatch_source}" "TryDevelopmentFactory(" direct_loader_dispatch_marker)
    if(direct_loader_dispatch_marker EQUAL -1)
        message(FATAL_ERROR "The real GetLoader dispatch must include the reviewed optional factory hook")
    endif()
    get_target_property(direct_core_sources core SOURCES)
    if(NOT "${direct_core_sources}" MATCHES "development_factory\\.cpp")
        message(FATAL_ERROR "The reviewed development factory implementation must be compiled into core")
    endif()
    foreach(read_boundary IN ITEMS
            "src/core/hle/service/filesystem/fsp/fs_i_storage.cpp"
            "src/core/file_sys/fsa/fs_i_file.h")
        file(READ "${PROJECT_SOURCE_DIR}/${read_boundary}" direct_read_boundary_source)
        string(FIND "${direct_read_boundary_source}" "catch (const FileSys::VfsReadError&" direct_read_boundary_marker)
        if(direct_read_boundary_marker EQUAL -1)
            # The Fsa header is already in FileSys, so it uses an unqualified type.
            string(FIND "${direct_read_boundary_source}" "catch (const VfsReadError&" direct_read_boundary_marker)
        endif()
        if(direct_read_boundary_marker EQUAL -1)
            message(FATAL_ERROR "Apply the dedicated read-error boundary patch: ${read_boundary}")
        endif()
    endforeach()
    get_target_property(direct_frontend_sources eden-headless SOURCES)
    get_target_property(direct_frontend_source_root eden-headless SOURCE_DIR)
    set(direct_frontend_route_found FALSE)
    foreach(frontend_source IN LISTS direct_frontend_sources)
        get_filename_component(frontend_source_absolute "${frontend_source}" ABSOLUTE
            BASE_DIR "${direct_frontend_source_root}")
        if(frontend_source_absolute MATCHES "[/\\]headless[/\\]main\\.cpp$" AND EXISTS "${frontend_source_absolute}")
            file(READ "${frontend_source_absolute}" direct_frontend_source)
            string(FIND "${direct_frontend_source}" "research.PerformLoadOnly()" direct_frontend_route_marker)
            if(NOT direct_frontend_route_marker EQUAL -1)
                set(direct_frontend_route_found TRUE)
            endif()
        endif()
    endforeach()
    if(NOT direct_frontend_route_found)
        message(FATAL_ERROR "Apply the reviewed Prospero main.cpp research route patch before enabling it")
    endif()
    set(EDEN_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
    set(ZIPVFS_COMMON_VFS_INCLUDE_ROOT "${PROJECT_SOURCE_DIR}/src")
    set(ZIPVFS_MINIZ_TARGET prosperoeden-miniz)
    include("${direct_loader_root}/../zip-vfs-prototype/zip_library.cmake")
    # Match Prospero's real Core layouts/includes and resolve ordinary VFS base
    # symbols through core rather than linking the host test support archive.
    target_include_directories(eden_zip_vfs BEFORE PUBLIC "$<TARGET_PROPERTY:core,INCLUDE_DIRECTORIES>")
    target_compile_definitions(eden_zip_vfs PRIVATE "$<TARGET_PROPERTY:core,COMPILE_DEFINITIONS>")
    target_compile_options(eden_zip_vfs PRIVATE -fno-rtti)
    target_link_libraries(eden_zip_vfs PUBLIC core)
    target_sources(prospero_direct_development_loader PRIVATE
        "${direct_loader_root}/research_descriptor.cpp"
        "${direct_loader_root}/research_frontend.cpp")
    target_link_libraries(prospero_direct_development_loader PUBLIC eden_zip_vfs)
    target_link_libraries(eden-headless PRIVATE prospero_direct_development_loader)
    target_compile_definitions(eden-headless PRIVATE PROSPERO_DIRECT_RESEARCH=1)
    if(NOT TARGET eden-metadata)
        message(FATAL_ERROR "Folder-game library integration requires the real eden-metadata target")
    endif()
    target_compile_definitions(eden-metadata PRIVATE PROSPERO_DIRECT_RESEARCH=1)
    target_link_libraries(eden-metadata PRIVATE prospero_direct_development_loader)
    if(PROSPERO_NATIVE_STARTUP_RESEARCH)
        target_compile_definitions(core PRIVATE PROSPERO_NATIVE_STARTUP_RESEARCH=1)
        target_compile_definitions(eden-headless PRIVATE PROSPERO_NATIVE_STARTUP_RESEARCH=1)
        target_compile_definitions(prospero_direct_development_loader PRIVATE PROSPERO_NATIVE_STARTUP_RESEARCH=1)
    endif()
    if(PROSPERO_DIRECT_ALLOW_EXECUTE)
        target_compile_definitions(prospero_direct_development_loader PRIVATE PROSPERO_DIRECT_ALLOW_EXECUTE=1)
        message(WARNING "Research execute requested: host/Core runtime and console compatibility are unvalidated")
    endif()
elseif(PROSPERO_DIRECT_ALLOW_EXECUTE)
    message(FATAL_ERROR "PROSPERO_DIRECT_ALLOW_EXECUTE requires PROSPERO_DIRECT_FRONTEND_ROUTE")
endif()

# Host-only load/mapping probe. It does not call System::Load, System::Run or
# Service::Process::Run, avoiding automatic setup/start of guest application code.
if(NOT PS5_NATIVE)
    add_executable(prospero_direct_load_only "${direct_loader_root}/load_only.cpp")
    target_include_directories(prospero_direct_load_only PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(prospero_direct_load_only PRIVATE prospero_direct_development_loader)
    target_compile_options(prospero_direct_load_only PRIVATE -fno-rtti)
endif()
