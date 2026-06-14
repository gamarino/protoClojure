cmake_minimum_required(VERSION 3.20)
project(protoClojure VERSION 0.0.1 LANGUAGES CXX)

# Default to RelWithDebInfo so casual builds are not -O0 (matches the protoST
# convention). Override with -DCMAKE_BUILD_TYPE=Release for a stripped -O3.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# --- protoCore (lifted from protoST's pattern) ---
# Either point at an installed protoCore via PROTO_CORE_PREFIX, or rely on the
# sibling source tree at ../protoCore with one of its build_* directories
# already populated.
if(DEFINED PROTO_CORE_PREFIX)
    find_library(PROTOCORE_LIBRARY NAMES protoCore
        HINTS "${PROTO_CORE_PREFIX}" PATH_SUFFIXES lib lib64)
    find_path(PROTO_CORE_INCLUDE_DIR NAMES protoCore.h
        HINTS "${PROTO_CORE_PREFIX}" PATH_SUFFIXES include headers)
    if(NOT (PROTOCORE_LIBRARY AND PROTO_CORE_INCLUDE_DIR))
        message(FATAL_ERROR
            "PROTO_CORE_PREFIX=${PROTO_CORE_PREFIX} set but protoCore not found.")
    endif()
    set(PROTOCORE_INCLUDE_DIRS "${PROTO_CORE_INCLUDE_DIR}")
else()
    set(PROTOCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protoCore)
    find_library(PROTOCORE_LIBRARY NAMES protoCore
        PATHS ${PROTOCORE_DIR}/build_release ${PROTOCORE_DIR}/build ${PROTOCORE_DIR}/build_check
        NO_DEFAULT_PATH)
    if(NOT PROTOCORE_LIBRARY)
        message(FATAL_ERROR
            "protoCore shared library not found. Build it first:\n"
            "  cd ${PROTOCORE_DIR} && cmake -B build_release -S . && cmake --build build_release --target protoCore")
    endif()
    set(PROTOCORE_INCLUDE_DIRS ${PROTOCORE_DIR} ${PROTOCORE_DIR}/headers)
    get_filename_component(PROTOCORE_LIB_DIR "${PROTOCORE_LIBRARY}" DIRECTORY)
    set(CMAKE_BUILD_RPATH "${PROTOCORE_LIB_DIR}")
    set(CMAKE_BUILD_RPATH_USE_LINK_PATH TRUE)
endif()

include_directories(${PROTOCORE_INCLUDE_DIRS} include headers src)

# --- protoClojure reader library ---
# A small static library so unit tests can link against it without rebuilding
# main.cpp. As phase 1 grows we will accumulate compiler/runtime/etc. into
# similarly small libraries here, then link the protoclj binary against the
# set.
add_library(protoclj_reader STATIC
    src/reader/Lexer.cpp
    src/reader/Reader.cpp
)
target_link_libraries(protoclj_reader PUBLIC ${PROTOCORE_LIBRARY})
target_compile_options(protoclj_reader PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure binary (minimum-viable so far) ---
add_executable(protoclj
    src/main.cpp
)
target_link_libraries(protoclj PRIVATE protoclj_reader)
target_compile_options(protoclj PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ---
enable_testing()
add_subdirectory(tests)

# --- Friendly info at configure time ---
message(STATUS "protoClojure ${PROJECT_VERSION}")
message(STATUS "protoCore lib: ${PROTOCORE_LIBRARY}")
message(STATUS "protoCore includes: ${PROTOCORE_INCLUDE_DIRS}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
