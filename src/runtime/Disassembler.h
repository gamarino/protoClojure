/*
 * Disassembler — a readable listing of a BytecodeModule (docs/VM.md).
 *
 * `protoclj --disasm script.clj` runs the script as usual and, before each
 * top-level form of the script runs, prints the form as it reaches the
 * compiler (after top-level macro expansion) and the bytecode of its module,
 * followed by every fn body it contains, nested ones included. clojure.core
 * (core.clj) is never listed.
 *
 * The listing names no addresses, so it is the same on every run and every
 * platform: docs/vm-examples/ keeps golden copies of it, which a ctest
 * compares (docs/vm-examples/disasm_examples.py --check).
 *
 * Listing layout:
 *
 *   ;; form 1: (+ 1 2)
 *   module form1: arity 0, locals 0, max stack 2
 *     constants:
 *       #0  long    1
 *     code:
 *       0  PUSH_CONST     0    ; 1
 *       ...
 *
 * A fn body is listed after the module that holds it, named by its path
 * (`form1.0` is block 0 of form 1, `form1.0.2` block 2 of that body).
 */
#pragma once

#include <cstdio>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

class BytecodeModule;

// The listing of `mod` and of every block it holds, recursively. `name` is
// the module's name in the listing (`form3`). `ctx` prints quoted data
// constants; it allocates only in `ctx`.
std::string disassemble(proto::ProtoContext* ctx, const BytecodeModule& mod,
                        const std::string& name);

// Where `--disasm` listings go: nullptr (the default) lists nothing.
void setDisassemblyOutput(std::FILE* out);
std::FILE* disassemblyOutput();

// Called by evalTopLevel for each top-level module it is about to run:
// prints `form` (as the compiler received it) and the listing of `mod` to the
// disassembly output, numbering the forms from 1. Does nothing when no output
// is set or while clojure.core loads. `form` must be rooted by the caller.
void disassembleTopLevel(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                         const BytecodeModule& mod);

} // namespace protoClojure
