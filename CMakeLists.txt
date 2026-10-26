cmake_minimum_required(VERSION 3.20)
project(protoClojure VERSION 0.0.3 LANGUAGES CXX)

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
    # /W4 for protoClojure's own targets, the counterpart of -Wall -Wextra:
    # it includes the truncation warnings (C4244, C4267) that /W3 reports
    # only in part, which matter where `long` is 32 bits.
    set(PROTOCLJ_WARNINGS /W4)
else()
    set(PROTOCLJ_WARNINGS -Wall -Wextra -Wpedantic)
endif()
# CI builds with warnings as errors, so a new warning fails the build instead
# of scrolling by. Off by default: a newer compiler must not break a user's
# build.
option(PROTOCLJ_WARNINGS_AS_ERRORS "Treat warnings in protoClojure's own sources as errors" OFF)
if(PROTOCLJ_WARNINGS_AS_ERRORS)
    if(MSVC)
        list(APPEND PROTOCLJ_WARNINGS /WX)
    else()
        list(APPEND PROTOCLJ_WARNINGS -Werror)
    endif()
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
# 3.0.0: methods receive their arguments as ProtoArguments /
# ProtoKeywordArguments descriptors (protoCore docs/CALLING_CONVENTION.md).
# 3.1.0: lazy objects (ProtoObject::newLazyChild), which lazy sequences are
# (src/runtime/LazySeq.h, docs/DESIGN.md M28).
# 3.3.0: ProtoLinkedList, which Clojure lists are (src/runtime/SeqTypes.h,
# docs/DESIGN.md M47).
set(PROTOCORE_MIN_VERSION      "3.3.0")
set(PROTOCORE_MIN_VERSION_FULL "3.3.0")
set(PROTOCORE_NEXT_MAJOR       "4.0.0")
set(PROTOCORE_ABI_SOVERSION    "4")
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

# protoCore's headers are a dependency's: SYSTEM, so the warning level of
# protoClojure's own sources does not apply to them.
include_directories(SYSTEM ${PROTOCORE_INCLUDE_DIRS})

# protoCore's adaptive heap controller (2.10.0+), which protoclj enables on its
# space (src/main.cpp). Below 2.10.0 -- the floor is 2.7.0 -- protoclj builds
# without it, and the tests that check it (tests/CMakeLists.txt) are not
# registered.
set(PROTOCLJ_HAS_ADAPTIVE_HEAP OFF)
foreach(_dir IN LISTS PROTOCORE_INCLUDE_DIRS)
    if(EXISTS "${_dir}/protoCore.h")
        file(STRINGS "${_dir}/protoCore.h" _ah REGEX "^#define PROTOCORE_HAS_ADAPTIVE_HEAP ")
        if(_ah)
            set(PROTOCLJ_HAS_ADAPTIVE_HEAP ON)
        endif()
        break()
    endif()
endforeach()
unset(_dir)
unset(_ah)
message(STATUS "protoClojure: adaptive heap controller: ${PROTOCLJ_HAS_ADAPTIVE_HEAP}")
include_directories(include headers src)

# --- protoIO: files, processes, TCP, UDP, TLS and HTTP (src/runtime/IO.cpp) ---
# The POSIX I/O layer shared by the protoCore runtimes (protoST, protoScala,
# protoClojure), linked statically, so the package gains no dependency on
# protoIO itself. Resolution order, the same as for protoCore: an installed
# CMake package (the protoio-dev deb, any prefix given with
# -DCMAKE_PREFIX_PATH, or a build tree given with -DprotoIO_DIR), else the
# sibling source tree ../protoIO built as part of this one. OpenSSL (TLS for
# sockets and https) comes with protoIO's PUBLIC link interface;
# dpkg-shlibdeps turns it into the package's libssl dependency. 0.2.3 is the
# floor: readBufferedLine, which line-seq and read-line use (IO.cpp).
find_package(protoIO 0.2.3 CONFIG QUIET)
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

# --- PCRE2: the regular-expression engine (src/runtime/Regex.cpp, M48) -------
# The 8-bit library, 10.34 or later (PCRE2_MATCH_INVALID_UTF), with its JIT
# where the platform has one. Resolution order: the system's library through
# pkg-config (libpcre2-dev on Debian / Ubuntu, pcre2-devel on Fedora / RHEL,
# pcre2 from Homebrew), then an installed PCRE2 CMake package (vcpkg, a
# prefix given with -DCMAKE_PREFIX_PATH), else PCRE2's pinned release built
# here as a static library (the Windows build, and any system without the
# package; -DPROTOCLJ_BUNDLED_PCRE2=ON forces it). The bundled copy is linked
# in, so it adds no file to any package; its install rules are kept out of
# protoClojure's component.
option(PROTOCLJ_BUNDLED_PCRE2 "Build PCRE2's pinned release instead of using the system's" OFF)
set(PROTOCLJ_PCRE2_TARGET "")
if(NOT PROTOCLJ_BUNDLED_PCRE2)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PROTOCLJ_PCRE2 QUIET IMPORTED_TARGET libpcre2-8>=10.34)
        if(PROTOCLJ_PCRE2_FOUND)
            set(PROTOCLJ_PCRE2_TARGET PkgConfig::PROTOCLJ_PCRE2)
            message(STATUS "protoClojure: PCRE2 ${PROTOCLJ_PCRE2_VERSION} from pkg-config")
        endif()
    endif()
    if(NOT PROTOCLJ_PCRE2_TARGET)
        find_package(PCRE2 10.34 CONFIG QUIET COMPONENTS 8BIT)
        if(TARGET PCRE2::8BIT)
            set(PROTOCLJ_PCRE2_TARGET PCRE2::8BIT)
            message(STATUS "protoClojure: PCRE2 ${PCRE2_VERSION} CMake package from ${PCRE2_DIR}")
        endif()
    endif()
endif()
if(NOT PROTOCLJ_PCRE2_TARGET)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_LIBBZ2 OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_LIBZ OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_LIBEDIT OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_LIBREADLINE OFF CACHE BOOL "" FORCE)
    set(PCRE2_STATIC_PIC ON CACHE BOOL "" FORCE)
    # Its install rules (headers, the archive, man pages) go to a component
    # of their own, which CPACK_COMPONENTS_ALL leaves out.
    set(_protoclj_default_component "${CMAKE_INSTALL_DEFAULT_COMPONENT_NAME}")
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME pcre2-bundled)
    # EXCLUDE_FROM_ALL (CMake 3.28): only what protoclj links is built.
    set(_protoclj_pcre2_exclude "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        set(_protoclj_pcre2_exclude EXCLUDE_FROM_ALL)
    endif()
    FetchContent_Declare(pcre2
        URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.46/pcre2-10.46.tar.bz2
        URL_HASH SHA256=15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f
        ${_protoclj_pcre2_exclude})
    unset(_protoclj_pcre2_exclude)
    FetchContent_MakeAvailable(pcre2)
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "${_protoclj_default_component}")
    unset(_protoclj_default_component)
    set(PROTOCLJ_PCRE2_TARGET pcre2-8-static)
    message(STATUS "protoClojure: PCRE2 10.46 built from its release (static, JIT)")
endif()

# --- protoClojure support library (native stack guard, bulk list builder,
# value representation) ---
# Used by the reader, the compiler and the runtime alike, so the reader does
# not depend on the runtime. The value representation (keywords and symbols,
# vectors, maps) is here because the reader yields runtime values: it builds
# the same vectors, maps and named values the runtime manipulates.
add_library(protoclj_support STATIC
    src/runtime/CallerSlots.cpp
    src/runtime/ListBuilder.cpp
    src/runtime/MapOps.cpp
    src/runtime/Named.cpp
    src/runtime/Regex.cpp
    src/runtime/ScopedListBuilder.cpp
    src/runtime/SeqTypes.cpp
    src/runtime/SetOps.cpp
    src/runtime/StackGuard.cpp
    src/runtime/VectorOps.cpp
)
target_link_libraries(protoclj_support PUBLIC ${PROTOCORE_LIBRARY} PRIVATE ${PROTOCLJ_PCRE2_TARGET})
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
    src/runtime/CorePrims.cpp
    src/runtime/DebugHook.cpp
    src/runtime/Disassembler.cpp
    src/runtime/Edn.cpp
    src/runtime/Exceptions.cpp
    src/runtime/ExecutionEngine.cpp
    src/runtime/GCCensus.cpp
    src/runtime/IO.cpp
    src/runtime/Json.cpp
    src/runtime/LazySeq.cpp
    src/runtime/Macros.cpp
    src/runtime/Primitives.cpp
)
# The runtime reads data with the reader (read-string, clojure.edn).
target_link_libraries(protoclj_runtime PUBLIC ${PROTOCORE_LIBRARY} protoclj_support protoclj_reader
                                        PRIVATE protoIO::protoio)
target_compile_options(protoclj_runtime PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure compiler library ---
# core.clj, the part of clojure.core written in Clojure, is embedded in the
# binary (src/compiler/Eval.cpp, loadCore), so protoclj needs no file beside
# it: the deb, the tarball and the build tree all run the same core.
set(PROTOCLJ_CORE_CLJ "${CMAKE_CURRENT_SOURCE_DIR}/src/core/core.clj")
set(PROTOCLJ_CORE_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/core_clj.inc")
add_custom_command(
    OUTPUT "${PROTOCLJ_CORE_INC}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${PROTOCLJ_CORE_CLJ} -DOUTPUT=${PROTOCLJ_CORE_INC}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS "${PROTOCLJ_CORE_CLJ}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding src/core/core.clj"
    VERBATIM)

add_library(protoclj_compiler STATIC
    src/compiler/Compiler.cpp
    src/compiler/Eval.cpp
    "${PROTOCLJ_CORE_INC}"
)
target_include_directories(protoclj_compiler PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(protoclj_compiler PUBLIC protoclj_runtime protoclj_reader)
target_compile_options(protoclj_compiler PRIVATE ${PROTOCLJ_WARNINGS})

# --- protoClojure debug adapter (`protoclj --dap`, docs/debugging.md) ---
# The Debug Adapter Protocol's messages are JSON: nlohmann/json, header-only,
# used at build time only. An installed package (libnlohmann-json3-dev, vcpkg,
# Homebrew) is used when there is one; otherwise the release's headers are
# fetched.
find_package(nlohmann_json 3.2 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
    FetchContent_MakeAvailable(nlohmann_json)
    # A dependency's headers: the warning level of protoClojure's own
    # sources does not apply to them.
    set_target_properties(nlohmann_json PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "$<TARGET_PROPERTY:nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
endif()
add_library(protoclj_dap STATIC
    src/dap/DapServer.cpp
    src/dap/DapStdio.cpp
    src/dap/DapTransport.cpp
)
target_link_libraries(protoclj_dap PUBLIC protoclj_compiler protoclj_runtime protoclj_reader
                                    nlohmann_json::nlohmann_json)
target_compile_options(protoclj_dap PRIVATE ${PROTOCLJ_WARNINGS})

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
    protoclj_dap
    protoclj_repl
    protoclj_reader
    protoclj_compiler
    protoclj_runtime)
target_compile_options(protoclj PRIVATE ${PROTOCLJ_WARNINGS})

# --- Geometry test binary (protoCore's example UMD module) -----------------
# protoclj with protoCore's example geometry module (protoCore
# examples/exceptions/, provider alias `example_geometry`) linked in: the same
# src/main.cpp, which registers the provider and puts it first in the space's
# resolution chain when PROTOCLJ_EXAMPLE_GEOMETRY is defined. It runs the
# Clojure test tests/modules/geometry-exceptions.clj, which imports the module
# with `(import-module "example/geometry")` and catches its error types. Built
# only against the developer protoCore tree, where the example library is.
if(DEFINED PROTOCORE_LIB_DIR)
    find_library(PROTOCLJ_GEOMETRY_LIBRARY NAMES protocore_example_geometry
        PATHS "${PROTOCORE_LIB_DIR}/examples/exceptions" NO_DEFAULT_PATH)
    find_path(PROTOCLJ_GEOMETRY_INCLUDE_DIR NAMES geometry_module/GeometryModule.h
        PATHS "${PROTOCORE_DIR}/examples/exceptions" NO_DEFAULT_PATH)
endif()
if(PROTOCLJ_GEOMETRY_LIBRARY AND PROTOCLJ_GEOMETRY_INCLUDE_DIR)
    add_executable(protoclj_geometry src/main.cpp)
    target_compile_definitions(protoclj_geometry PRIVATE PROTOCLJ_EXAMPLE_GEOMETRY)
    target_include_directories(protoclj_geometry PRIVATE "${PROTOCLJ_GEOMETRY_INCLUDE_DIR}")
    target_link_libraries(protoclj_geometry PRIVATE
        protoclj_dap protoclj_repl protoclj_reader protoclj_compiler protoclj_runtime
        "${PROTOCLJ_GEOMETRY_LIBRARY}")
    target_compile_options(protoclj_geometry PRIVATE ${PROTOCLJ_WARNINGS})
    message(STATUS "protoClojure: geometry module test enabled (${PROTOCLJ_GEOMETRY_LIBRARY})")
else()
    message(STATUS "protoClojure: protoCore's example geometry module not found; "
                   "the module exception test is not built")
endif()

# Windows finds a DLL next to the program that loads it. Copy the DLLs
# protoClojure needs -- protoCore, and OpenSSL through protoIO -- into the
# build's bin/, so protoclj and the unit tests run in place.
#
# protoCore's DLL is taken from the imported target's location, whatever its
# file name (it may carry its ABI version). OpenSSL's are named for the
# version that was found (OpenSSL 3 for x64: libcrypto-3-x64.dll and
# libssl-3-x64.dll, beside its include directory); configuring fails when they
# are missing rather than packaging whatever a pattern happens to match, and
# OpenSSL's licence file, which must ship with its DLLs, is located as well.
if(WIN32)
    set(PROTOCLJ_RUNTIME_DLLS "")
    if(TARGET ${PROTOCORE_LIBRARY})
        list(APPEND PROTOCLJ_RUNTIME_DLLS "$<TARGET_FILE:${PROTOCORE_LIBRARY}>")
    endif()
    # The OpenSSL protoIO links (found again here: protoIO's own find_package
    # may have run in its subdirectory's scope).
    find_package(OpenSSL REQUIRED)
    string(REGEX MATCH "^[0-9]+" PROTOCLJ_OPENSSL_MAJOR "${OPENSSL_VERSION}")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PROTOCLJ_OPENSSL_SUFFIX "-${PROTOCLJ_OPENSSL_MAJOR}-x64")
    else()
        set(PROTOCLJ_OPENSSL_SUFFIX "-${PROTOCLJ_OPENSSL_MAJOR}")
    endif()
    get_filename_component(PROTOCLJ_OPENSSL_ROOT "${OPENSSL_INCLUDE_DIR}/.." ABSOLUTE)
    set(PROTOCLJ_OPENSSL_DLLS
        "${PROTOCLJ_OPENSSL_ROOT}/bin/libcrypto${PROTOCLJ_OPENSSL_SUFFIX}.dll"
        "${PROTOCLJ_OPENSSL_ROOT}/bin/libssl${PROTOCLJ_OPENSSL_SUFFIX}.dll")
    foreach(_dll IN LISTS PROTOCLJ_OPENSSL_DLLS)
        if(NOT EXISTS "${_dll}")
            message(FATAL_ERROR
                "protoClojure: OpenSSL ${OPENSSL_VERSION} was found in ${PROTOCLJ_OPENSSL_ROOT}, "
                "but its DLL ${_dll} is missing. Point OPENSSL_ROOT_DIR at an OpenSSL "
                "installation that has its DLLs in bin/.")
        endif()
    endforeach()
    set(PROTOCLJ_OPENSSL_LICENSE "" CACHE FILEPATH
        "OpenSSL's licence file, shipped with its DLLs (found under OPENSSL_ROOT_DIR when empty)")
    if(NOT PROTOCLJ_OPENSSL_LICENSE)
        find_file(PROTOCLJ_OPENSSL_LICENSE_FOUND
            NAMES LICENSE.txt LICENSE license.txt
            PATHS "${PROTOCLJ_OPENSSL_ROOT}" "${PROTOCLJ_OPENSSL_ROOT}/doc"
                  "${PROTOCLJ_OPENSSL_ROOT}/share/doc/openssl"
            NO_DEFAULT_PATH NO_CACHE)
        set(PROTOCLJ_OPENSSL_LICENSE "${PROTOCLJ_OPENSSL_LICENSE_FOUND}")
    endif()
    if(NOT PROTOCLJ_OPENSSL_LICENSE OR NOT EXISTS "${PROTOCLJ_OPENSSL_LICENSE}")
        message(FATAL_ERROR
            "protoClojure: OpenSSL's licence file was not found under ${PROTOCLJ_OPENSSL_ROOT}; "
            "it must ship with the OpenSSL DLLs. Name it with -DPROTOCLJ_OPENSSL_LICENSE=<file>.")
    endif()
    message(STATUS "protoClojure: OpenSSL ${OPENSSL_VERSION} DLLs from ${PROTOCLJ_OPENSSL_ROOT}/bin, "
                   "licence ${PROTOCLJ_OPENSSL_LICENSE}")
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

# --- Version consistency ---
# protoclj prints the constants of headers/protoClojure.h, so they must match
# project() above; configuring fails when one is bumped without the other.
file(STRINGS "${CMAKE_SOURCE_DIR}/headers/protoClojure.h" PROTOCLJ_VERSION_LINES
    REGEX "constexpr int kVersion(Major|Minor|Patch) = [0-9]+;")
set(PROTOCLJ_HEADER_VERSION "")
foreach(part Major Minor Patch)
    foreach(line IN LISTS PROTOCLJ_VERSION_LINES)
        if(line MATCHES "kVersion${part} = ([0-9]+);")
            list(APPEND PROTOCLJ_HEADER_VERSION "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()
list(JOIN PROTOCLJ_HEADER_VERSION "." PROTOCLJ_HEADER_VERSION)
if(NOT PROTOCLJ_HEADER_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "headers/protoClojure.h declares version "
        "'${PROTOCLJ_HEADER_VERSION}' but project() declares ${PROJECT_VERSION}; "
        "update both.")
endif()

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
# Windows: the installed tree, and so the ZIP and the installer, is
# self-contained. protoIO is linked in statically, but protoCore and OpenSSL
# are DLLs, and Windows looks for a DLL next to the program first: both go
# next to protoclj.exe (protoCore's whatever its file name), with OpenSSL's
# licence, and so does the MSVC runtime (InstallRequiredSystemLibraries:
# vcruntime140.dll, msvcp140.dll, ...), so protoclj.exe runs on a machine
# without the Visual C++ Redistributable.
if(WIN32)
    if(TARGET ${PROTOCORE_LIBRARY})
        install(FILES "$<TARGET_FILE:${PROTOCORE_LIBRARY}>"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT protoClojure)
    endif()
    install(FILES ${PROTOCLJ_OPENSSL_DLLS}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT protoClojure)
    install(FILES "${PROTOCLJ_OPENSSL_LICENSE}"
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME OpenSSL-LICENSE.txt
        COMPONENT protoClojure)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT protoClojure)
    include(InstallRequiredSystemLibraries)
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
    USE_SOURCE_PERMISSIONS
    COMPONENT protoClojure
    FILES_MATCHING PATTERN "*.clj"
    PATTERN ".vscode" EXCLUDE)

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
    # A plain ZIP, self-contained (protoclj.exe, protoCore's and OpenSSL's
    # DLLs, the MSVC runtime; see the install rules above). CI unpacks it
    # into a clean directory and runs it with nothing of the build on PATH.
    # The NSIS installer is added only when makensis is found, since cpack
    # aborts the whole run when a generator's tool is missing; it is
    # configured but has not been verified.
    set(CPACK_GENERATOR "ZIP")
    set(_pf86 "ProgramFiles(x86)")
    find_program(PROTOCLOJURE_MAKENSIS_EXECUTABLE makensis
        PATHS "$ENV{${_pf86}}/NSIS" "$ENV{ProgramFiles}/NSIS")
    unset(_pf86)
    if(PROTOCLOJURE_MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        message(STATUS "CPack: NSIS generator enabled (makensis found)")
    else()
        message(STATUS "CPack: NSIS generator disabled (makensis not found)")
    endif()
    set(CPACK_NSIS_HELP_LINK     "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/gamarino/protoClojure")
    set(CPACK_NSIS_PACKAGE_NAME  "protoClojure")
endif()

include(CPack)
