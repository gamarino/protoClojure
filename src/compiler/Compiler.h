/*
 * Compiler — turns a Clojure form (a ProtoObject tree from the Reader) into
 * a BytecodeModule.
 *
 * P1/P2: the compiler receives a ProtoContext from its caller and walks
 * the form's children. Every intermediate ProtoObject* it inspects is
 * either a parameter (rooted by the caller's slot, P1) or a result of
 * getAttribute/asList/getAt on an already-rooted parameter (returns from
 * protoCore methods that are immediately used — we do not hold them
 * across allocations). No child contexts pushed in v0.0.x — the compiler
 * is non-recursive in the protoCore-allocation sense; it consumes existing
 * forms and emits bytecode.
 *
 * P3: writes go to a BytecodeModule, which is C++-side. Const-pool values
 * are POD (long long) or std::string; they never enter protoCore until
 * the VM materialises them at PUSH_CONST time, via raw-bytes c_str().
 *
 * Session 3 scope: integers, strings, symbols (resolved to PUSH_VAR), and
 * lists interpreted as call forms. No special forms yet — that is
 * session 4.
 */
#pragma once
#include "runtime/BytecodeModule.h"

#include <stdexcept>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
}

namespace protoClojure {

struct CompileError : std::runtime_error {
    CompileError(const std::string& msg) : std::runtime_error(msg) {}
};

// Mirror of ReaderMarkers — the Compiler needs the same two markers to
// recognise wrapped string literals coming back from the Reader. See the
// ReaderMarkers comment in src/reader/Reader.h for the why.
struct CompilerMarkers {
    const proto::ProtoObject* stringMarkerProto;
    const proto::ProtoString* bytesKey;
};

class Compiler {
public:
    // Compile a single form, appending instructions to `out`. Does NOT
    // terminate the module — the file runner calls `compileForm` once per
    // top-level form, then appends a final RETURN.
    void compileForm(proto::ProtoContext* ctx,
                     const proto::ProtoObject* form,
                     BytecodeModule& out,
                     const CompilerMarkers& markers);
};

} // namespace protoClojure
