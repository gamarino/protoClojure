# --- GoogleTest via FetchContent (same pattern as protoCore) -----------------
# Suppress the CMP0135 warning about FetchContent timestamps.
cmake_policy(SET CMP0135 NEW)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
if(MSVC)
    # Link GoogleTest against the same (DLL) C runtime as the rest of the build.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(googletest)

add_subdirectory(unit)

# Phase 1 testing scaffolding.
#
# Conformance fixtures: discovered automatically from tests/conformance/**.clj.
# Each one is a self-contained Clojure source file with a directive comment in
# the first line (`;; EXPECT: <text>` or `;; EXPECT-ERROR[: <text>]`). The
# runner script invokes the protoclj binary and compares.
#
# GoogleTest unit tests for the sub-language internals land here in later
# sessions (lexer, reader, bytecode module). For v0.0.x we only register the
# conformance scaffolding so the suite shape is in place from the first build.

# --- Conformance fixtures (glob-discovered) -----------------------------------
file(GLOB_RECURSE PROTOCLJ_CONFORMANCE_FILES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/conformance"
    "${CMAKE_CURRENT_SOURCE_DIR}/conformance/*.clj")

# --- CLI checks ----------------------------------------------------------------
# The script tests are bash scripts. Windows cannot run a script directly, so
# there they run through the bash of Git for Windows (not System32's bash.exe,
# which is WSL); elsewhere the prefix is empty and the script is the command.
if(WIN32)
    find_program(PROTOCLJ_BASH bash HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin"
                 NO_DEFAULT_PATH)
    if(NOT PROTOCLJ_BASH)
        find_program(PROTOCLJ_BASH bash)
    endif()
    set(PROTOCLJ_SH "${PROTOCLJ_BASH}")
else()
    set(PROTOCLJ_SH "")
endif()
# --- Adaptive heap controller (the default memory policy, src/main.cpp) --------
# The driver reads protoCore's per-cycle trace (PROTOCORE_HEAP_TRACE=1) from the
# child's standard error and measures the child's peak RSS. Python 3 drives the
# checks because it measures a child's peak RSS on Linux, macOS and Windows
# alike (bash cannot); without a Python 3 interpreter, or against a protoCore
# without the controller (< 2.10.0), they are not registered.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND PROTOCLJ_HAS_ADAPTIVE_HEAP)
    set(_ah_check "${CMAKE_CURRENT_SOURCE_DIR}/cli/adaptive_heap_check.py")
    set(_ah_prog "${CMAKE_CURRENT_SOURCE_DIR}/cli/adaptive-heap-garbage.clj")
    # On by default: nothing in the environment; 20000 rounds allocate about
    # 14 M cells, well past the controller's initial 2 M-cell soft limit.
    add_test(NAME cli/adaptive-heap-on-by-default
        COMMAND ${Python3_EXECUTABLE} "${_ah_check}" --expect on
            --sentinel "adaptive heap garbage done"
            --unset PROTOCORE_ADAPTIVE_HEAP --unset PROTOCORE_HEAP_LIMIT_CELLS
            --unset PROTOCORE_ADAPTIVE_HEAP_START
            -- "$<TARGET_FILE:protoclj>" "${_ah_prog}" 20000)
    # Off with PROTOCORE_ADAPTIVE_HEAP=0: the budget becomes a fixed hard limit.
    add_test(NAME cli/adaptive-heap-off-by-env
        COMMAND ${Python3_EXECUTABLE} "${_ah_check}" --expect off
            --sentinel "adaptive heap garbage done"
            --set PROTOCORE_ADAPTIVE_HEAP=0 --set PROTOCORE_HEAP_LIMIT_CELLS=1000000
            --unset PROTOCORE_ADAPTIVE_HEAP_START
            -- "$<TARGET_FILE:protoclj>" "${_ah_prog}" 20000)
    # Bounded memory: a 1 M-cell budget (64 MiB of cells) and 60000 rounds,
    # about 40 times that in garbage; at least 10 cycles, and the peak RSS
    # bounded by the budget plus the interpreter's own footprint.
    add_test(NAME cli/adaptive-heap-bounded-memory
        COMMAND ${Python3_EXECUTABLE} "${_ah_check}" --expect on
            --sentinel "adaptive heap garbage done"
            --set PROTOCORE_HEAP_LIMIT_CELLS=1000000
            --unset PROTOCORE_ADAPTIVE_HEAP --unset PROTOCORE_ADAPTIVE_HEAP_START
            --min-cycles 10 --max-rss-mb 160
            -- "$<TARGET_FILE:protoclj>" "${_ah_prog}" 60000)
    set_tests_properties(cli/adaptive-heap-on-by-default cli/adaptive-heap-off-by-env
        cli/adaptive-heap-bounded-memory PROPERTIES TIMEOUT 300)
    unset(_ah_check)
    unset(_ah_prog)
endif()

add_test(NAME cli/help
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/help.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/large-program
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/large-program.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/bulk-builders-under-heap-limit
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/bulk-builders-under-heap-limit.sh" "$<TARGET_FILE:protoclj>")
# A `loop` is one call frame, so its garbage is reclaimed only if the VM hands
# the frame's young generation to the collector at the back-edge. The fixture
# is two-directional: it also runs the workload with the safepoint hook
# disabled and fails if THAT run succeeds.
add_test(NAME cli/loop-garbage-is-reclaimed
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/loop-garbage-is-reclaimed.sh" "$<TARGET_FILE:protoclj>")
# `ProtoThread::join` does not leave protoCore's running set, so a thread that
# blocks there holds the stop-the-world quorum and no collection can start.
# Four cases, one per join site, each under a heap ceiling so a cycle is needed.
add_test(NAME cli/blocking-joins-park-for-gc
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/blocking-joins-park-for-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/actor-payloads-survive-gc
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/actor-payloads-survive-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-stack-overflow
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-stack-overflow.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-nil-global
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-nil-global.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-literals
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-literals.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/reader-values-under-heap-limit
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/reader-values-under-heap-limit.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-quote
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-quote.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/quoted-data-survive-gc
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/quoted-data-survive-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-macros
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-macros.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/macro-expansion-survives-gc
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/macro-expansion-survives-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/file-form-by-form
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/file-form-by-form.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-forms-in-order
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-forms-in-order.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/deeply-nested-source
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/deeply-nested-source.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/uncaught-exception
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/uncaught-exception.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/exceptions-survive-gc
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/exceptions-survive-gc.sh" "$<TARGET_FILE:protoclj>")
# Input and output (src/runtime/IO.cpp): standard input in a pipeline, the
# script's arguments, exit statuses, and an HTTP server under a heap ceiling.
add_test(NAME cli/io-stdin-read-line
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/io-stdin-read-line.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/io-command-line-args
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/io-command-line-args.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/io-exit-codes
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/io-exit-codes.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/io-sh-dir-env-native
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/io-sh-dir-env-native.sh" "$<TARGET_FILE:protoclj>")
# Scripts with a `#!/usr/bin/env protoclj` line run as commands in pipelines.
add_test(NAME cli/shebang-scripts
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/shebang-scripts.sh" "$<TARGET_FILE:protoclj>")
# A server without a host answers ::1, so "localhost" is served at once.
add_test(NAME cli/http-localhost-latency
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/http-localhost-latency.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/http-server-under-heap-limit
    COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/cli/http-server-under-heap-limit.sh" "$<TARGET_FILE:protoclj>")

# Git Bash rewrites a POSIX path given to a native program as an argument, and
# turns /dev/stdin into /proc/self/fd/0, which protoclj.exe cannot open; the
# CLI checks pipe their programs in through /dev/stdin, so that argument is
# passed as it is (src/main.cpp reads it as standard input on Windows).
if(WIN32)
    get_property(PROTOCLJ_CLI_TESTS DIRECTORY PROPERTY TESTS)
    set_tests_properties(${PROTOCLJ_CLI_TESTS} PROPERTIES
        ENVIRONMENT "MSYS2_ARG_CONV_EXCL=/dev/stdin")
endif()

foreach(conf_file ${PROTOCLJ_CONFORMANCE_FILES})
    # Module-helper files prefixed with `_` are loaded by other fixtures, not
    # run themselves (same convention protoST uses).
    get_filename_component(basename "${conf_file}" NAME)
    string(SUBSTRING "${basename}" 0 1 first_char)
    if(NOT "${first_char}" STREQUAL "_")
        add_test(NAME "conformance/${conf_file}"
            COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/conformance/run.sh"
                    "$<TARGET_FILE:protoclj>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/conformance/${conf_file}")
    endif()
endforeach()

# --- Module exceptions (protoCore's example geometry module) -------------------
# A Clojure program run by protoclj_geometry (CMakeLists.txt), which links the
# module's provider: it imports `example/geometry` and catches the module's
# error types by name, protoCore's errors as Clojure classes, and a C++
# exception from the module as a RuntimeException.
if(TARGET protoclj_geometry)
    add_test(NAME modules/geometry-exceptions.clj
        COMMAND ${PROTOCLJ_SH} "${CMAKE_CURRENT_SOURCE_DIR}/conformance/run.sh"
                "$<TARGET_FILE:protoclj_geometry>"
                "${CMAKE_CURRENT_SOURCE_DIR}/modules/geometry-exceptions.clj")
endif()
