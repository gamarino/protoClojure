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
