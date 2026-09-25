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

# --- protoCore -----------------------------------------------------------------
# Resolution order (Phase I, decision D-I1):
#   1. An installed protoCore CMake package: find_package(protoCore 2.1 CONFIG).
#      The only mode that checks protoCore's version and ABI, and therefore the
#      only mode a distributable package may be built in. Name a prefix with
#      -DPROTO_CORE_PREFIX=<prefix> or -DCMAKE_PREFIX_PATH=<prefix>.
#   2. Developer fallback: the sibling source tree ../protoCore with one of its
#      build_* directories already populated. Used only when no installed
#      package was found and no prefix was named; refused by
#      -DPROTOCORE_REQUIRE_PACKAGE=ON.
#
# The version floor is 2.1: protoClojure's maps are protoCore `ProtoMap`s
# driven through its hashed-collection helper, both added in 2.1.0. The
# ceiling is the next major, because protoCore's major version and its soname
# move together.
set(PROTOCORE_MIN_VERSION      "2.1")
set(PROTOCORE_MIN_VERSION_FULL "2.1.0")
set(PROTOCORE_NEXT_MAJOR       "3.0.0")
set(PROTOCORE_ABI_SOVERSION    "3")
option(PROTOCORE_REQUIRE_PACKAGE
    "Require an installed protoCore CMake package; never fall back to ../protoCore" OFF)

if(DEFINED PROTO_CORE_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${PROTO_CORE_PREFIX}")
endif()

find_package(protoCore ${PROTOCORE_MIN_VERSION} QUIET CONFIG)

if(protoCore_FOUND)
    if(NOT protoCore_SOVERSION STREQUAL PROTOCORE_ABI_SOVERSION)
        message(FATAL_ERROR
            "protoCore package at ${protoCore_DIR} declares SOVERSION "
            "${protoCore_SOVERSION}; protoClojure is built against SOVERSION "
            "${PROTOCORE_ABI_SOVERSION}. Install a matching protoCore.")
    endif()
    set(PROTOCORE_LIBRARY protoCore::protoCore)
    set(PROTOCORE_INCLUDE_DIRS "${protoCore_INCLUDE_DIR}")
    message(STATUS "protoClojure: installed protoCore ${protoCore_VERSION} "
                   "(SOVERSION ${protoCore_SOVERSION}) from ${protoCore_DIR}")
else()
    set(_protoCore_where "")
    if(DEFINED PROTO_CORE_PREFIX)
        set(_protoCore_where " under ${PROTO_CORE_PREFIX}")
    endif()
    if(DEFINED PROTO_CORE_PREFIX OR PROTOCORE_REQUIRE_PACKAGE)
        message(FATAL_ERROR
            "No protoCore >= ${PROTOCORE_MIN_VERSION_FULL} CMake package was "
            "found${_protoCore_where}.\n"
            "  A prefix holding only libprotoCore and protoCore.h is no longer "
            "accepted: without lib/cmake/protoCore/protoCoreConfig.cmake there "
            "is no way to tell protoCore 1.x from 2.x. Install protoCore "
            "${PROTOCORE_MIN_VERSION_FULL} or later:\n"
            "    cmake -S ../protoCore -B ../protoCore/build_release "
            "-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<prefix>\n"
            "    cmake --build ../protoCore/build_release --target protoCore\n"
            "    cmake --install ../protoCore/build_release --component protoCore")
    endif()
    unset(_protoCore_where)
    message(WARNING
        "protoClojure: no installed protoCore package found; using the "
        "developer build in ../protoCore. This mode performs no package "
        "version check and must not be used to produce a distributable "
        "package (configure with -DPROTOCORE_REQUIRE_PACKAGE=ON to forbid it).")
    set(PROTOCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protoCore)
    find_library(PROTOCORE_LIBRARY NAMES protoCore
        PATHS ${PROTOCORE_DIR}/build_release ${PROTOCORE_DIR}/build ${PROTOCORE_DIR}/build_check
        NO_DEFAULT_PATH)
    if(NOT PROTOCORE_LIBRARY)
        message(FATAL_ERROR
            "protoCore shared library not found. Build it first:\n"
            "  cd ${PROTOCORE_DIR} && cmake -B build_release -S . && cmake --build build_release --target protoCore")
    endif()
    get_filename_component(PROTOCORE_LIB_DIR "${PROTOCORE_LIBRARY}" DIRECTORY)
    if(APPLE)
        set(_protoCore_abi_file "${PROTOCORE_LIB_DIR}/libprotoCore.${PROTOCORE_ABI_SOVERSION}.dylib")
    else()
        set(_protoCore_abi_file "${PROTOCORE_LIB_DIR}/libprotoCore.so.${PROTOCORE_ABI_SOVERSION}")
    endif()
    if(NOT EXISTS "${_protoCore_abi_file}")
        message(FATAL_ERROR
            "The protoCore build in ${PROTOCORE_LIB_DIR} does not provide "
            "${_protoCore_abi_file}, so it is not SOVERSION "
            "${PROTOCORE_ABI_SOVERSION}. Rebuild ../protoCore from clean.")
    endif()
    unset(_protoCore_abi_file)
    set(PROTOCORE_INCLUDE_DIRS ${PROTOCORE_DIR} ${PROTOCORE_DIR}/headers)
    set(CMAKE_BUILD_RPATH "${PROTOCORE_LIB_DIR}")
    set(CMAKE_BUILD_RPATH_USE_LINK_PATH TRUE)
endif()

include_directories(${PROTOCORE_INCLUDE_DIRS} include headers src)

# --- protoClojure support library (native stack guard, bulk list builder) ---
# Used by the reader, the compiler and the runtime alike, so the reader does
# not depend on the runtime.
add_library(protoclj_support STATIC
    src/runtime/ListBuilder.cpp
    src/runtime/StackGuard.cpp
)
target_link_libraries(protoclj_support PUBLIC ${PROTOCORE_LIBRARY})
target_compile_options(protoclj_support PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure reader library ---
add_library(protoclj_reader STATIC
    src/reader/Lexer.cpp
    src/reader/Reader.cpp
)
target_link_libraries(protoclj_reader PUBLIC ${PROTOCORE_LIBRARY} protoclj_support)
target_compile_options(protoclj_reader PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure runtime library (bytecode, VM, primitives) ---
add_library(protoclj_runtime STATIC
    src/runtime/ActorScheduler.cpp
    src/runtime/BytecodeModule.cpp
    src/runtime/ExecutionEngine.cpp
    src/runtime/MapOps.cpp
    src/runtime/VectorOps.cpp
    src/runtime/Named.cpp
    src/runtime/Primitives.cpp
)
target_link_libraries(protoclj_runtime PUBLIC ${PROTOCORE_LIBRARY} protoclj_support)
target_compile_options(protoclj_runtime PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure compiler library ---
add_library(protoclj_compiler STATIC
    src/compiler/Compiler.cpp
)
target_link_libraries(protoclj_compiler PUBLIC protoclj_runtime)
target_compile_options(protoclj_compiler PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure REPL library ---
# libreadline is a hard requirement of the REPL (history, arrow-key
# editing, multi-line continuation prompt). System libreadline-dev is
# present on every modern Linux + macOS Homebrew distribution.
find_library(READLINE_LIBRARY NAMES readline)
find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h)
if(NOT READLINE_LIBRARY OR NOT READLINE_INCLUDE_DIR)
    message(FATAL_ERROR
        "libreadline not found. Install it (Debian/Ubuntu: libreadline-dev; "
        "Fedora/RHEL: readline-devel; macOS: brew install readline).")
endif()
add_library(protoclj_repl STATIC
    src/repl/Repl.cpp
)
target_include_directories(protoclj_repl PRIVATE "${READLINE_INCLUDE_DIR}")
target_link_libraries(protoclj_repl
    PUBLIC  protoclj_compiler protoclj_reader protoclj_runtime
    PRIVATE "${READLINE_LIBRARY}")
target_compile_options(protoclj_repl PRIVATE -Wall -Wextra -Wpedantic)

# --- protoClojure binary ---
add_executable(protoclj
    src/main.cpp
)
target_link_libraries(protoclj PRIVATE
    protoclj_repl
    protoclj_reader
    protoclj_compiler
    protoclj_runtime)
target_compile_options(protoclj PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ---
enable_testing()
add_subdirectory(tests)

# --- Friendly info at configure time ---
message(STATUS "protoClojure ${PROJECT_VERSION}")
message(STATUS "protoCore lib: ${PROTOCORE_LIBRARY}")
message(STATUS "protoCore includes: ${PROTOCORE_INCLUDE_DIRS}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# --- Installation packaging (mirrors protoST Track 10) ---------------------
# An installed `bin/protoclj` must find `libprotoCore.so` (or `.dylib`) under
# `<prefix>/lib` without the user having to set LD_LIBRARY_PATH /
# DYLD_LIBRARY_PATH. We do that with INSTALL_RPATH; on Windows the loader
# searches the executable's own directory by default, so no RPATH is needed.
if(UNIX)
    if(APPLE)
        set_target_properties(protoclj PROPERTIES
            INSTALL_RPATH "@executable_path/../${CMAKE_INSTALL_LIBDIR}")
    else()
        set_target_properties(protoclj PROPERTIES
            INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

# The `protoclj` binary.
install(TARGETS protoclj
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT protoClojure)

# License and documentation (Markdown only — sources, specs, internal notes
# stay in the repo).
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT protoClojure)
install(FILES README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT protoClojure)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT protoClojure
    FILES_MATCHING PATTERN "*.md")

# Example `.clj` scripts (small, useful for users discovering the language
# from a freshly installed package).
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/protoClojure/examples
    COMPONENT protoClojure
    FILES_MATCHING PATTERN "*.clj")

# Benchmark scripts (so a user can reproduce the published numbers from a
# package install, without cloning the repo).
install(DIRECTORY benchmarks/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/protoClojure/benchmarks
    COMPONENT protoClojure
    FILES_MATCHING
    PATTERN "*.clj"
    PATTERN "*.sh"
    PATTERN "RESULTS.md")

# --- CPack configuration ---------------------------------------------------
set(CPACK_PACKAGE_NAME "protoclojure")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "protoClojure - a Clojure-flavoured language on the protoCore runtime")
set(CPACK_PACKAGE_VENDOR "numaes")
set(CPACK_PACKAGE_CONTACT "gamarino@gmail.com")
set(CPACK_RESOURCE_FILE_README  "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_COMPONENTS_ALL protoClojure)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "protoClojure")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/gamarino/protoClojure")

# Platform-specific generators. protoClojure links libprotoCore — every
# generator's package metadata declares it as a runtime dependency on
# protoCore's package (lowercased to "protocore" for DEB, kept as-is for
# RPM, mentioned in NSIS' help link for Windows).
if(APPLE)
    # macOS drag-and-drop .dmg.
    # UNVERIFIED: configured and reviewed, never built - no macOS host (D-I6).
    set(CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    # Enable only the generators whose tools are present: cpack aborts the whole
    # run when one of them is missing, which would take the TGZ down with it.
    # Same pattern as protoCore's own CPack block.
    set(CPACK_GENERATOR "TGZ")
    find_program(PROTOCLOJURE_DPKG_EXECUTABLE dpkg)
    find_program(PROTOCLOJURE_RPMBUILD_EXECUTABLE rpmbuild)
    if(PROTOCLOJURE_DPKG_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        message(STATUS "CPack: DEB generator enabled (dpkg found)")
    else()
        message(STATUS "CPack: DEB generator disabled (dpkg not found)")
    endif()
    if(PROTOCLOJURE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        message(STATUS "CPack: RPM generator enabled (rpmbuild found)")
    else()
        message(STATUS "CPack: RPM generator disabled (rpmbuild not found)")
    endif()
    # Names pinned rather than inferred from each generator's default casing;
    # the dependency is bounded on both sides because protoCore's major version
    # and its soname move together.
    set(CPACK_DEBIAN_PACKAGE_NAME "protoclojure")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "gamarino@gmail.com")
    set(CPACK_DEBIAN_PACKAGE_SECTION "interpreters")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "protocore (>= ${PROTOCORE_MIN_VERSION_FULL}), protocore (<< ${PROTOCORE_NEXT_MAJOR})")
    set(CPACK_RPM_PACKAGE_NAME "protoClojure")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
    set(CPACK_RPM_PACKAGE_REQUIRES
        "protoCore >= ${PROTOCORE_MIN_VERSION_FULL}, protoCore < ${PROTOCORE_NEXT_MAJOR}")
elseif(WIN32)
    # Windows NSIS installer + plain ZIP.
    # UNVERIFIED: configured and reviewed, never built - no Windows host (D-I6).
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_HELP_LINK     "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_PACKAGE_NAME  "protoClojure")
endif()

include(CPack)
