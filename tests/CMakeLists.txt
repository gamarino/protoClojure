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
