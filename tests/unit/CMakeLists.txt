# GoogleTest unit tests for sub-language internals (lexer, reader, …).
# The bulk of feature testing happens via tests/conformance/; these unit
# tests cover what is genuinely below the script-level interface.

add_executable(protoclj_unit_tests
    test_lexer.cpp
    test_reader.cpp
)

target_link_libraries(protoclj_unit_tests
    PRIVATE protoclj_reader gtest gtest_main)

target_compile_options(protoclj_unit_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(protoclj_unit_tests)
