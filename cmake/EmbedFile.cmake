# Writes the bytes of INPUT as a comma-separated list of C character
# constants to OUTPUT, for `static const char x[] = { #include "OUTPUT" 0 };`.
# Run with `cmake -DINPUT=... -DOUTPUT=... -P EmbedFile.cmake`; used to embed
# src/core/core.clj in protoclj (src/compiler/Eval.cpp).
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," body "${hex}")
string(REGEX REPLACE "(([^,]*,){16})" "\\1\n" body "${body}")
file(WRITE "${OUTPUT}.tmp" "${body}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
