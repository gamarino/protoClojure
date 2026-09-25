# --- GoogleTest via FetchContent (same pattern as protoCore) -----------------
# Suppress the CMP0135 warning about FetchContent timestamps.
cmake_policy(SET CMP0135 NEW)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
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
add_test(NAME cli/help
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/help.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/large-program
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/large-program.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/bulk-builders-under-heap-limit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/bulk-builders-under-heap-limit.sh" "$<TARGET_FILE:protoclj>")
# A `loop` is one call frame, so its garbage is reclaimed only if the VM hands
# the frame's young generation to the collector at the back-edge. The fixture
# is two-directional: it also runs the workload with the safepoint hook
# disabled and fails if THAT run succeeds.
add_test(NAME cli/loop-garbage-is-reclaimed
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/loop-garbage-is-reclaimed.sh" "$<TARGET_FILE:protoclj>")
# `ProtoThread::join` does not leave protoCore's running set, so a thread that
# blocks there holds the stop-the-world quorum and no collection can start.
# Four cases, one per join site, each under a heap ceiling so a cycle is needed.
add_test(NAME cli/blocking-joins-park-for-gc
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/blocking-joins-park-for-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/actor-payloads-survive-gc
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/actor-payloads-survive-gc.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-stack-overflow
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-stack-overflow.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/repl-nil-global
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/repl-nil-global.sh" "$<TARGET_FILE:protoclj>")
add_test(NAME cli/deeply-nested-source
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cli/deeply-nested-source.sh" "$<TARGET_FILE:protoclj>")

foreach(conf_file ${PROTOCLJ_CONFORMANCE_FILES})
    # Module-helper files prefixed with `_` are loaded by other fixtures, not
    # run themselves (same convention protoST uses).
    get_filename_component(basename "${conf_file}" NAME)
    string(SUBSTRING "${basename}" 0 1 first_char)
    if(NOT "${first_char}" STREQUAL "_")
        add_test(NAME "conformance/${conf_file}"
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/conformance/run.sh"
                    "$<TARGET_FILE:protoclj>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/conformance/${conf_file}")
    endif()
endforeach()
