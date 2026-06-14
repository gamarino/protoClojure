#include "Compiler.h"

#include "protoCore.h"

#include <cstdio>

namespace protoClojure {

namespace {

// Pointer-tag constants matching protoCore's internal layout. Defined here
// to avoid pulling proto_internal.h; copied from headers/proto_internal.h
// and stable across protoCore versions (they are baked into every cell).
constexpr unsigned int kTagList      = 2;   // POINTER_TAG_LIST
constexpr unsigned int kTagListSmall = 25;  // POINTER_TAG_LIST_SMALL

unsigned int tagOf(const proto::ProtoObject* form) {
    return static_cast<unsigned int>(
        reinterpret_cast<uintptr_t>(form) & 0x3F);
}

// True iff the form is a genuine ProtoList — large or small inline form.
// We do NOT use `asList(ctx)` here because it treats a ProtoString as a
// sequence of characters and returns a non-null ProtoList, which would
// make the compiler walk every char of every string literal as if it
// were a call form. The tag check is precise.
bool isList(const proto::ProtoObject* form) {
    if (!form) return false;
    unsigned int t = tagOf(form);
    return t == kTagList || t == kTagListSmall;
}

// Returns true if the form is a ProtoString of any kind — symbol-tagged
// (POINTER_TAG_SYMBOL = 22) or string-tagged (POINTER_TAG_STRING = 6).
// The protoCore `isString(ctx)` instance method returns false on
// symbol-tagged values; the tag-only `isStringTagFast` static accepts
// both. The Compiler does not need the distinction between a symbol and a
// string at this layer — it disambiguates by the form's *position* (head
// of a list ≡ var reference, top-level standalone ≡ literal).
bool isStringy(const proto::ProtoObject* form) {
    return form && proto::ProtoObject::isStringTagFast(form);
}

// Extract the std-side bytes of a ProtoString-tagged ProtoObject. Works
// for both string and symbol tags because both encode a ProtoString
// underneath. The toStdString cross is the same boundary every other call
// site uses (P3 — copy out via raw bytes, the std::string lives only on
// the C++ stack).
std::string asUtf8(proto::ProtoContext* ctx, const proto::ProtoObject* form) {
    const proto::ProtoString* s =
        reinterpret_cast<const proto::ProtoString*>(form);
    return s->toStdString(ctx);
}

} // namespace

void Compiler::compileForm(proto::ProtoContext* ctx,
                           const proto::ProtoObject* form,
                           BytecodeModule& out) {
    if (form == nullptr || form == PROTO_NONE) {
        // nil literal — not yet emitting a PUSH_NIL opcode. For session 3 we
        // represent nil as a const-pool entry with kind String "" until
        // PUSH_NIL lands. Cheating but bounded; nil is not exercised by
        // hello-world.
        throw CompileError("nil literal not yet implemented in v0.0.x");
    }

    // Integer literal — emit PUSH_CONST <addLong>.
    if (form->isInteger(ctx)) {
        std::size_t idx = out.addLong(form->asLong(ctx));
        if (idx > 255) {
            throw CompileError("const-pool overflow (>255 entries) — EXTEND "
                               "prefix not yet implemented in v0.0.x");
        }
        out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
        return;
    }

    // Call form: a non-empty list (head + args).
    if (isList(form)) {
        const proto::ProtoList* lst = form->asList(ctx);
        unsigned long n = lst->getSize(ctx);
        if (n == 0) {
            throw CompileError("empty () not yet implemented in v0.0.x");
        }
        // Push the callable (head): a symbol in v0.0.x.
        const proto::ProtoObject* head = lst->getAt(ctx, 0);
        if (!isStringy(head)) {
            throw CompileError("call head must be a symbol in v0.0.x");
        }
        std::string headName = asUtf8(ctx, head);
        std::size_t headIdx = out.addSymbol(headName);
        if (headIdx > 255) {
            throw CompileError("const-pool overflow on symbol");
        }
        out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(headIdx));

        // Push each argument.
        for (unsigned long i = 1; i < n; ++i) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out);
        }

        // CALL with argc = n - 1.
        unsigned long argc = n - 1;
        if (argc > 255) {
            throw CompileError("call with >255 args not supported in v0.0.x");
        }
        out.emit(Op::CALL, static_cast<std::uint8_t>(argc));
        return;
    }

    // String literal — fromUTF8String.
    if (isStringy(form)) {
        std::string s = asUtf8(ctx, form);
        std::size_t idx = out.addString(s);
        if (idx > 255) {
            throw CompileError("const-pool overflow on string");
        }
        out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
        return;
    }

    throw CompileError("compile: unsupported form in v0.0.x");
}

} // namespace protoClojure
