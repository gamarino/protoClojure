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

# Compiler and platform differences, gathered here so the targets below read the
# same on every platform. Outside Windows these are the flags the build always
# used.
if(MSVC)
    # Sources are UTF-8; the VM and primitives are large translation units.
    add_compile_options(/utf-8 /bigobj)
    # The sources mark inlining for GCC and Clang with gnu:: attributes, which
    # MSVC ignores (C5030), and use the standard C library (getenv, C4996).
    add_compile_options(/wd5030)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(PROTOCLJ_WARNINGS /W3)
else()
    set(PROTOCLJ_WARNINGS -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    # Windows has no rpath: a program finds its DLLs next to itself, so every
    # executable and DLL of the build goes to one directory.
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

# --- protoCore -----------------------------------------------------------------
# Resolution order (Phase I, decision D-I1):
#   1. An installed protoCore CMake package: find_package(protoCore 2.6.1 CONFIG).
#      The only mode that checks protoCore's version and ABI, and therefore the
#      only mode a distributable package may be built in. Name a prefix with
#      -DPROTO_CORE_PREFIX=<prefix> or -DCMAKE_PREFIX_PATH=<prefix>.
#   2. Developer fallback: the sibling source tree ../protoCore with one of its
#      build_* directories already populated. Used only when no installed
#      package was found and no prefix was named; refused by
#      -DPROTOCORE_REQUIRE_PACKAGE=ON.
#
# The version floor is 2.6.1: the HTTP server creates a thread per connection
# from its accept thread, and before 2.6.1 ProtoSpace::newThread called from a
# thread other than the creator detached a live context's roots (a later cycle
# freed objects still in use). (Maps as `ProtoMap`s needed 2.1.0.) The
# ceiling is the next major, because protoCore's major version and its soname
# move together.
# 2.7.0: the Windows port spells protoCore's 64-bit integers
# proto::proto_long / proto_ulong, which first exist there.
set(PROTOCORE_MIN_VERSION      "2.7.0")
set(PROTOCORE_MIN_VERSION_FULL "2.7.0")
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
    if(DEFINED PROTO_CORE_PREFIX OR PROTOCORE_REQUIRE_PACKAGE OR WIN32)
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
            "    cmake --install ../protoCore/build_release --component protoCore\n"
            "  (On Windows only an installed package is supported: name its prefix "
            "with -DCMAKE_PREFIX_PATH=<prefix>.)")
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

# --- protoIO: files, processes, TCP, UDP, TLS and HTTP (src/runtime/IO.cpp) ---
# The POSIX I/O layer shared by the protoCore runtimes (protoST, protoScala,
# protoClojure), linked statically, so the package gains no dependency on
# protoIO itself. Resolution order, the same as for protoCore: an installed
# CMake package (the protoio-dev deb, any prefix given with
# -DCMAKE_PREFIX_PATH, or a build tree given with -DprotoIO_DIR), else the
# sibling source tree ../protoIO built as part of this one. OpenSSL (TLS for
# sockets and https) comes with protoIO's PUBLIC link interface;
# dpkg-shlibdeps turns it into the package's libssl dependency.
find_package(protoIO 0.2.1 CONFIG QUIET)
if(protoIO_FOUND)
    message(STATUS "protoClojure: installed protoIO ${protoIO_VERSION} from ${protoIO_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../protoIO/CMakeLists.txt")
    message(STATUS "protoClojure: no installed protoIO package found; building the sibling ../protoIO")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../protoIO"
                     "${CMAKE_CURRENT_BINARY_DIR}/_protoIO" EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR
        "protoIO not found: install the protoio-dev package, or check out "
        "https://github.com/gamarino/protoIO next to protoClojure (../protoIO).")
endif()

# --- protoClojure support library (native stack guard, bulk list builder) ---
# Used by the reader, the compiler and the runtime alike, so the reader does
# not depend on the runtime.
add_library(protoclj_support STATIC
    src/runtime/ListBuilder.cpp
    src/runtime/StackGuard.cpp
)
target_link_libraries(protoclj_support PUBLIC ${PROTOCORE_LIBRARY})
target_compile_options(protoclj_support PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure reader library ---
add_library(protoclj_reader STATIC
    src/reader/Lexer.cpp
    src/reader/Reader.cpp
)
target_link_libraries(protoclj_reader PUBLIC ${PROTOCORE_LIBRARY} protoclj_support)
target_compile_options(protoclj_reader PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure runtime library (bytecode, VM, primitives) ---
add_library(protoclj_runtime STATIC
    src/runtime/ActorScheduler.cpp
    src/runtime/BytecodeModule.cpp
    src/runtime/Exceptions.cpp
    src/runtime/ExecutionEngine.cpp
    src/runtime/GCCensus.cpp
    src/runtime/IO.cpp
    src/runtime/MapOps.cpp
    src/runtime/VectorOps.cpp
    src/runtime/Named.cpp
    src/runtime/Primitives.cpp
)
target_link_libraries(protoclj_runtime PUBLIC ${PROTOCORE_LIBRARY} protoclj_support
                                        PRIVATE protoIO::protoio)
target_compile_options(protoclj_runtime PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure compiler library ---
add_library(protoclj_compiler STATIC
    src/compiler/Compiler.cpp
)
target_link_libraries(protoclj_compiler PUBLIC protoclj_runtime)
target_compile_options(protoclj_compiler PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure REPL library ---
# libreadline is a hard requirement of the REPL (history, arrow-key
# editing, multi-line continuation prompt). System libreadline-dev is
# present on every modern Linux + macOS Homebrew distribution.
find_library(READLINE_LIBRARY NAMES readline)
find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h)
if(WIN32 AND (NOT READLINE_LIBRARY OR NOT READLINE_INCLUDE_DIR))
    # The Windows console edits the line and keeps a history itself, so the
    # REPL reads plain lines there (src/repl/Repl.cpp, PROTOCLJ_NO_READLINE).
    message(STATUS "protoClojure: no readline; the REPL uses the console's own line editing")
    set(PROTOCLJ_NO_READLINE ON)
elseif(NOT READLINE_LIBRARY OR NOT READLINE_INCLUDE_DIR)
    message(FATAL_ERROR
        "libreadline not found. Install it (Debian/Ubuntu: libreadline-dev; "
        "Fedora/RHEL: readline-devel; macOS: brew install readline).")
endif()
add_library(protoclj_repl STATIC
    src/repl/Repl.cpp
)
if(PROTOCLJ_NO_READLINE)
    target_compile_definitions(protoclj_repl PRIVATE PROTOCLJ_NO_READLINE)
    target_link_libraries(protoclj_repl
        PUBLIC  protoclj_compiler protoclj_reader protoclj_runtime)
else()
    target_include_directories(protoclj_repl PRIVATE "${READLINE_INCLUDE_DIR}")
    target_link_libraries(protoclj_repl
        PUBLIC  protoclj_compiler protoclj_reader protoclj_runtime
        PRIVATE "${READLINE_LIBRARY}")
endif()
target_compile_options(protoclj_repl PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure binary ---
add_executable(protoclj
    src/main.cpp
)
if(WIN32)
    # UTF-8 process code page: argv, getenv and paths are UTF-8 as elsewhere.
    target_sources(protoclj PRIVATE src/windows/utf8.manifest)
endif()
if(MSVC)
    # The Windows counterpart of configureThreadStacks (StackGuard.h): every
    # thread created without an explicit size (std::thread, so protoCore's
    # future, pmap and actor threads) reserves the stack size in the
    # executable's header. 32 MiB = protoClojure::kThreadStackBytes; pages are
    # committed only as a recursion touches them.
    target_link_options(protoclj PRIVATE /STACK:33554432)
endif()
target_link_libraries(protoclj PRIVATE
    protoclj_repl
    protoclj_reader
    protoclj_compiler
    protoclj_runtime)
target_compile_options(protoclj PRIVATE ${PROTOCLJ_WARNINGS})

# Windows finds a DLL next to the program that loads it. Copy the DLLs
# protoClojure needs -- protoCore, and OpenSSL through protoIO -- into the
# build's bin/, so protoclj and the unit tests run in place.
if(WIN32)
    set(PROTOCLJ_RUNTIME_DLLS "")
    if(TARGET ${PROTOCORE_LIBRARY})
        list(APPEND PROTOCLJ_RUNTIME_DLLS "$<TARGET_FILE:${PROTOCORE_LIBRARY}>")
    endif()
    set(PROTOCLJ_OPENSSL_DLLS "")
    if(OPENSSL_INCLUDE_DIR)
        get_filename_component(PROTOCLJ_OPENSSL_BIN "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
        file(GLOB PROTOCLJ_OPENSSL_DLLS
            "${PROTOCLJ_OPENSSL_BIN}/libcrypto*.dll" "${PROTOCLJ_OPENSSL_BIN}/libssl*.dll")
    endif()
    list(APPEND PROTOCLJ_RUNTIME_DLLS ${PROTOCLJ_OPENSSL_DLLS})
    # A target every library depends on, not a post-build step of protoclj:
    # the unit tests are run to list their tests as soon as they are linked.
    add_custom_target(protoclj_runtime_dlls
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROTOCLJ_RUNTIME_DLLS}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        VERBATIM)
    add_dependencies(protoclj_support protoclj_runtime_dlls)
endif()

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
# protoIO is linked in statically, but OpenSSL is a DLL on Windows: install it
# next to protoclj.exe, where Windows looks for it. (protoCore is a package of
# its own and installs its DLL into its own prefix's bin/.)
if(WIN32 AND PROTOCLJ_OPENSSL_DLLS)
    install(FILES ${PROTOCLJ_OPENSSL_DLLS}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT protoClojure)
endif()

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
    # SHLIBDEPS is meant to add a SONAME dependency (libprotoCore.so.N)
    # derived from the binary's own NEEDED entry, because the version range
    # above is not an ABI check on its own: a protoCore in range but built
    # with an older SOVERSION is accepted by dpkg and then fails at load
    # time (installers-sep2026 report, F4). Verified it does NOT add that
    # dependency: rebuilding this package's DEB with the flag on produced
    # Depends: protocore (>= 2.6.1), protocore (<< 3.0.0), libc6 (>= 2.38),
    # libgcc-s1 (>= 3.0), libreadline8t64 (>= 6.0), libstdc++6 (>= 13) -- no
    # protocore SONAME entry. Cause, confirmed by hand-running dpkg-shlibdeps
    # --ignore-missing-info against the staged binary: `dpkg -S` resolves
    # libprotoCore.so.3 to the protocore package, but that package ships no
    # shlibs/symbols control file, so dpkg-shlibdeps has no version data to
    # emit for it, and CPack's default --ignore-missing-info silently drops
    # the entry instead of failing the build. Left on because it is harmless
    # and adds real transitive deps (libc6, libstdc++6, ...); closing F4
    # needs a shlibs or symbols file in protoCore's own package.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_RPM_PACKAGE_NAME "protoClojure")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
    set(CPACK_RPM_PACKAGE_REQUIRES
        "protoCore >= ${PROTOCORE_MIN_VERSION_FULL}, protoCore < ${PROTOCORE_NEXT_MAJOR}")
elseif(WIN32)
    # Windows NSIS installer + plain ZIP.
    # The ZIP was built and run on Windows 11 / MSVC 19.44 (`cpack -G ZIP`:
    # protoclj.exe and the OpenSSL DLLs; protoCore's DLL comes with protoCore's
    # own package). The NSIS installer is configured but has not been built.
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_HELP_LINK     "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_PACKAGE_NAME  "protoClojure")
endif()

include(CPack)
