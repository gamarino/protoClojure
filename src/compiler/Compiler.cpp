#include "Compiler.h"

#include "protoCore.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

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

// True iff `form` is a Reader-wrapped string literal (its prototype is the
// Reader's stringMarkerProto). The wrapped bytes are extracted via
// getAttribute(bytesKey).
static bool isWrappedString(proto::ProtoContext* ctx,
                            const proto::ProtoObject* form,
                            const CompilerMarkers& markers) {
    if (!form) return false;
    const proto::ProtoObject* proto = form->getPrototype(ctx);
    return proto == markers.stringMarkerProto;
}

std::unique_ptr<BytecodeModule>
Compiler::compileFnBody(proto::ProtoContext* ctx,
                        const proto::ProtoList* fnForm,
                        const CompilerMarkers& markers) {
    // Detect whether the form is (fn [params] body...) or
    // (defn name [params] body...). The compileForm dispatcher routes
    // both here and we discover the shape from the head's name.
    unsigned long n = fnForm->getSize(ctx);
    const proto::ProtoObject* head = fnForm->getAt(ctx, 0);
    std::string headName = asUtf8(ctx, head);
    unsigned long paramsAt = 0;
    if (headName == "fn")        paramsAt = 1;
    else if (headName == "defn") paramsAt = 2;
    else throw CompileError("compileFnBody: head must be `fn` or `defn`");

    if (n <= paramsAt) throw CompileError("fn/defn: missing parameter vector");
    const proto::ProtoObject* paramsForm = fnForm->getAt(ctx, (int)paramsAt);
    if (!isList(paramsForm)) {
        throw CompileError("fn/defn: parameter list must be a vector");
    }
    const proto::ProtoList* params = paramsForm->asList(ctx);
    unsigned long arity = params->getSize(ctx);

    auto body = std::make_unique<BytecodeModule>();
    body->setArity(static_cast<int>(arity));

    // Push a fresh fn scope. Params go in slots 0..arity-1.
    scopes_.push_back(Scope{});
    Scope& scope = scopes_.back();
    scope.arity = static_cast<int>(arity);
    for (unsigned long i = 0; i < arity; ++i) {
        const proto::ProtoObject* p = params->getAt(ctx, (int)i);
        if (!isStringy(p)) {
            scopes_.pop_back();
            throw CompileError("fn/defn: parameter name must be a symbol");
        }
        scope.nameToSlot[asUtf8(ctx, p)] = static_cast<int>(i);
    }
    scope.nextSlot = static_cast<int>(arity);

    // Compile the body forms after the params vector.
    unsigned long bodyStart = paramsAt + 1;
    if (bodyStart >= n) {
        // Empty body → return nil.
        body->emit(Op::PUSH_NIL, 0);
    } else {
        for (unsigned long i = bodyStart; i < n; ++i) {
            compileForm(ctx, fnForm->getAt(ctx, (int)i), *body, markers);
            if (i + 1 < n) body->emit(Op::POP, 0);
        }
    }
    body->emit(Op::RETURN, 0);

    body->setLocalCount(scope.nextSlot - scope.arity);
    scopes_.pop_back();
    return body;
}

int Compiler::lookupLocal(const std::string& name) const {
    if (scopes_.empty()) return -1;
    const auto& top = scopes_.back();
    auto it = top.nameToSlot.find(name);
    if (it == top.nameToSlot.end()) return -1;
    return it->second;
}

void Compiler::compileForm(proto::ProtoContext* ctx,
                           const proto::ProtoObject* form,
                           BytecodeModule& out,
                           const CompilerMarkers& markers) {
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

    // List form: either a special form or a call form. We decide by the
    // head symbol's text: a fixed set of names dispatch as special forms,
    // anything else is a plain call.
    if (isList(form)) {
        const proto::ProtoList* lst = form->asList(ctx);
        unsigned long n = lst->getSize(ctx);
        if (n == 0) {
            throw CompileError("empty () not yet implemented in v0.0.x");
        }
        const proto::ProtoObject* head = lst->getAt(ctx, 0);
        if (!isStringy(head)) {
            throw CompileError("call head must be a symbol in v0.0.x");
        }
        std::string headName = asUtf8(ctx, head);

        // ---- Special forms -------------------------------------------------

        // (def name expr) — compile expr, store in globals under `name`.
        // The store leaves the value on the stack as `def`'s result, matching
        // Clojure semantics (def returns the Var; we return the value for
        // session 4 simplicity — close enough for the conformance tests).
        if (headName == "def") {
            if (n != 3) throw CompileError("def: expects (def name expr)");
            const proto::ProtoObject* nameForm = lst->getAt(ctx, 1);
            const proto::ProtoObject* exprForm = lst->getAt(ctx, 2);
            if (!isStringy(nameForm)) {
                throw CompileError("def: name must be a symbol");
            }
            compileForm(ctx, exprForm, out, markers);
            std::size_t nameIdx = out.addSymbol(asUtf8(ctx, nameForm));
            if (nameIdx > 255) throw CompileError("def: const-pool overflow");
            out.emit(Op::STORE_GLOBAL, static_cast<std::uint8_t>(nameIdx));
            return;
        }

        // (if test then else?) — JUMP_IF_FALSE over then, JUMP over else.
        // Offsets are back-patched once the branch sizes are known.
        if (headName == "if") {
            if (n != 3 && n != 4) {
                throw CompileError("if: expects (if test then) or (if test then else)");
            }
            compileForm(ctx, lst->getAt(ctx, 1), out, markers);  // test
            std::size_t jifAt = out.emit(Op::JUMP_IF_FALSE, 0);

            compileForm(ctx, lst->getAt(ctx, 2), out, markers);  // then
            std::size_t jmpAt = out.emit(Op::JUMP, 0);

            // patch the JIF to point at the start of the else branch
            std::size_t elseStart = out.pos();
            std::size_t elseOffsetInstr =
                (elseStart - (jifAt + kInstrSize)) / kInstrSize;
            if (elseOffsetInstr > 255) {
                throw CompileError("if: then-branch too large for 1-byte offset (v0.0.x)");
            }
            out.patchOperand(jifAt,
                static_cast<std::uint8_t>(elseOffsetInstr));

            if (n == 4) {
                compileForm(ctx, lst->getAt(ctx, 3), out, markers);  // else
            } else {
                out.emit(Op::PUSH_NIL, 0);                  // implicit nil
            }

            // patch the JMP to point past the else branch
            std::size_t afterElse = out.pos();
            std::size_t pastOffsetInstr =
                (afterElse - (jmpAt + kInstrSize)) / kInstrSize;
            if (pastOffsetInstr > 255) {
                throw CompileError("if: else-branch too large for 1-byte offset (v0.0.x)");
            }
            out.patchOperand(jmpAt,
                static_cast<std::uint8_t>(pastOffsetInstr));
            return;
        }

        // (do form1 form2 ... formN) — evaluate each form, POP between for
        // statement-level discard. The last form's value is the do's value.
        if (headName == "do") {
            if (n == 1) {
                out.emit(Op::PUSH_NIL, 0);     // (do) => nil
                return;
            }
            for (unsigned long i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
                if (i + 1 < n) out.emit(Op::POP, 0);
            }
            return;
        }

        // (quote form) — push the form as a literal. For v0.0.x we only
        // support quoting atoms (numbers, strings, symbols). Quoting a list
        // requires materialising the data structure at runtime, which is a
        // later-session feature.
        if (headName == "quote") {
            if (n != 2) throw CompileError("quote: expects (quote form)");
            const proto::ProtoObject* q = lst->getAt(ctx, 1);
            if (isList(q)) {
                throw CompileError("quote: list quoting not yet in v0.0.x");
            }
            // Atom: emit as a PUSH_CONST. Symbol is the interesting case —
            // we add it to the const pool as Symbol kind so PUSH_CONST
            // materialises an interned symbol, not a string.
            if (isWrappedString(ctx, q, markers)) {
                const proto::ProtoObject* raw =
                    q->getAttribute(ctx, markers.bytesKey);
                std::size_t idx = out.addString(
                    reinterpret_cast<const proto::ProtoString*>(raw)
                        ->toStdString(ctx));
                if (idx > 255) throw CompileError("quote: const-pool overflow");
                out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
                return;
            }
            if (isStringy(q)) {
                std::size_t idx = out.addSymbol(asUtf8(ctx, q));
                if (idx > 255) throw CompileError("quote: const-pool overflow");
                out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
                return;
            }
            if (q->isInteger(ctx)) {
                std::size_t idx = out.addLong(q->asLong(ctx));
                if (idx > 255) throw CompileError("quote: const-pool overflow");
                out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
                return;
            }
            throw CompileError("quote: unsupported atom in v0.0.x");
        }

        // (fn [params] body...) — compile body into a sub-module, emit
        // MAKE_FN with the sub-module index. Session 5: no closures; the
        // fn captures nothing from the enclosing lexical scope.
        if (headName == "fn") {
            std::unique_ptr<BytecodeModule> body =
                compileFnBody(ctx, lst, markers);
            std::size_t blockIdx = out.addBlock(std::move(body));
            if (blockIdx > 255) {
                throw CompileError("fn: too many fn bodies (>255) in module");
            }
            out.emit(Op::MAKE_FN, static_cast<std::uint8_t>(blockIdx));
            return;
        }

        // (defn name [params] body...) — sugar for (def name (fn [params]
        // body...)). Implement directly to avoid building an intermediate
        // protoCore list.
        if (headName == "defn") {
            if (n < 3) {
                throw CompileError("defn: expects (defn name [params] body...)");
            }
            const proto::ProtoObject* nameForm = lst->getAt(ctx, 1);
            if (!isStringy(nameForm)) {
                throw CompileError("defn: name must be a symbol");
            }
            // Build the equivalent fn-form pieces (params + body) and reuse
            // compileFnBody. We pass the full `lst` plus the param-start
            // index so compileFnBody can reach into the form.
            std::unique_ptr<BytecodeModule> body =
                compileFnBody(ctx, lst, markers);  // expects head to be
                                                    // skipped, params at idx 2
            std::size_t blockIdx = out.addBlock(std::move(body));
            if (blockIdx > 255) {
                throw CompileError("defn: too many fn bodies (>255) in module");
            }
            out.emit(Op::MAKE_FN, static_cast<std::uint8_t>(blockIdx));
            std::size_t nameIdx = out.addSymbol(asUtf8(ctx, nameForm));
            if (nameIdx > 255) throw CompileError("defn: const-pool overflow");
            out.emit(Op::STORE_GLOBAL, static_cast<std::uint8_t>(nameIdx));
            return;
        }

        // (let [name1 expr1 name2 expr2 ...] body...) — sequentially bind
        // each name to its expr in a freshly-allocated local slot, then
        // compile the body. Bindings are visible to subsequent binding RHSes
        // (Clojure's let* semantics).
        if (headName == "let") {
            if (scopes_.empty()) {
                throw CompileError("let: only supported inside fn in v0.0.x");
            }
            if (n < 2) throw CompileError("let: expects bindings vector");
            const proto::ProtoObject* bindings = lst->getAt(ctx, 1);
            if (!isList(bindings)) {
                throw CompileError("let: bindings must be a vector");
            }
            const proto::ProtoList* bvec = bindings->asList(ctx);
            unsigned long bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("let: bindings must come in name/value pairs");
            }
            Scope& scope = scopes_.back();
            for (unsigned long i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isStringy(nameForm)) {
                    throw CompileError("let: binding name must be a symbol");
                }
                compileForm(ctx, valForm, out, markers);  // push value
                int slot = scope.nextSlot++;
                if (slot > 255) {
                    throw CompileError("let: local-slot overflow (>255)");
                }
                scope.nameToSlot[asUtf8(ctx, nameForm)] = slot;
                out.emit(Op::STORE_LOCAL, static_cast<std::uint8_t>(slot));
            }
            // Body — last expression's value stays on the stack as `let`'s
            // value.
            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);                  // (let [...]) => nil
            } else {
                for (unsigned long i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }
            return;
        }

        // (loop [name1 expr1 ...] body...) — like let, plus a recur target
        // at the start of the body. recur rebinds the loop locals and jumps
        // back.
        if (headName == "loop") {
            if (scopes_.empty()) {
                throw CompileError("loop: only supported inside fn in v0.0.x");
            }
            if (n < 2) throw CompileError("loop: expects bindings vector");
            const proto::ProtoObject* bindings = lst->getAt(ctx, 1);
            if (!isList(bindings)) {
                throw CompileError("loop: bindings must be a vector");
            }
            const proto::ProtoList* bvec = bindings->asList(ctx);
            unsigned long bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("loop: bindings must come in name/value pairs");
            }
            Scope& scope = scopes_.back();
            std::vector<int> recurSlots;
            for (unsigned long i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isStringy(nameForm)) {
                    throw CompileError("loop: binding name must be a symbol");
                }
                compileForm(ctx, valForm, out, markers);
                int slot = scope.nextSlot++;
                if (slot > 255) {
                    throw CompileError("loop: local-slot overflow (>255)");
                }
                scope.nameToSlot[asUtf8(ctx, nameForm)] = slot;
                out.emit(Op::STORE_LOCAL, static_cast<std::uint8_t>(slot));
                recurSlots.push_back(slot);
            }
            // Recur target = current bytecode position. recur jumps BACK here
            // after rebinding.
            Scope::RecurTarget tgt;
            tgt.bodyStart = out.pos();
            tgt.slots = recurSlots;
            scope.recurStack.push_back(tgt);

            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);
            } else {
                for (unsigned long i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }

            scope.recurStack.pop_back();
            return;
        }

        // (recur arg1 arg2 ... argN) — rebind the enclosing loop's locals
        // and jump back to its body start. arity must match.
        if (headName == "recur") {
            if (scopes_.empty() || scopes_.back().recurStack.empty()) {
                throw CompileError("recur: no enclosing loop in current scope");
            }
            const auto& tgt = scopes_.back().recurStack.back();
            unsigned long argc = n - 1;
            if (argc != tgt.slots.size()) {
                throw CompileError("recur: arity mismatch with enclosing loop");
            }
            // Compile all args left-to-right; the last one ends on top of
            // the stack. Then store each into its slot in REVERSE order so
            // the rightmost (top-of-stack) value goes into the rightmost
            // slot.
            for (unsigned long i = 0; i < argc; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)(i + 1)), out, markers);
            }
            for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                int slot = tgt.slots[i];
                out.emit(Op::STORE_LOCAL, static_cast<std::uint8_t>(slot));
            }
            // JUMP_BACK: operand is the instruction count to subtract from
            // pc (pc was at the instruction AFTER JUMP_BACK when handler
            // runs — same arithmetic as JUMP / JUMP_IF_FALSE but in reverse).
            std::size_t jbAt = out.emit(Op::JUMP_BACK, 0);
            std::size_t backOffsetBytes = (jbAt + kInstrSize) - tgt.bodyStart;
            std::size_t backOffsetInstr = backOffsetBytes / kInstrSize;
            if (backOffsetInstr > 255) {
                throw CompileError("recur: loop body too large for 1-byte JUMP_BACK");
            }
            out.patchOperand(jbAt,
                static_cast<std::uint8_t>(backOffsetInstr));
            // recur never falls through; the bytecode after it is unreachable.
            return;
        }

        // ---- Plain call form -----------------------------------------------

        // The head is treated as a symbol reference. If it is bound in the
        // current fn scope (a let-bound function value), emit PUSH_LOCAL;
        // otherwise emit PUSH_VAR for a global lookup at runtime.
        int slot = lookupLocal(headName);
        if (slot >= 0) {
            out.emit(Op::PUSH_LOCAL, static_cast<std::uint8_t>(slot));
        } else {
            std::size_t headIdx = out.addSymbol(headName);
            if (headIdx > 255) {
                throw CompileError("const-pool overflow on symbol");
            }
            out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(headIdx));
        }

        // Push each argument.
        for (unsigned long i = 1; i < n; ++i) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
        }

        // CALL with argc = n - 1.
        unsigned long argc = n - 1;
        if (argc > 255) {
            throw CompileError("call with >255 args not supported in v0.0.x");
        }
        out.emit(Op::CALL, static_cast<std::uint8_t>(argc));
        return;
    }

    // Wrapped string literal — every string token comes back from the Reader
    // wrapped in a child of stringMarkerProto with the raw ProtoString stored
    // under bytesKey. We check the prototype to distinguish a string
    // literal from a (potentially-inline) symbol, since at the pointer-tag
    // level inline short content is indistinguishable.
    if (isWrappedString(ctx, form, markers)) {
        const proto::ProtoObject* raw =
            form->getAttribute(ctx, markers.bytesKey);
        if (!raw || !proto::ProtoObject::isStringTagFast(raw)) {
            throw CompileError("compile: malformed string-wrapper");
        }
        std::size_t idx = out.addString(
            reinterpret_cast<const proto::ProtoString*>(raw)->toStdString(ctx));
        if (idx > 255) throw CompileError("const-pool overflow on string");
        out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
        return;
    }

    // Bare stringy atom — by elimination this is a symbol reference. If it
    // is bound in the current fn scope (a param or a let-binding), emit
    // PUSH_LOCAL; otherwise fall back to a runtime global lookup.
    if (isStringy(form)) {
        std::string name = asUtf8(ctx, form);
        int slot = lookupLocal(name);
        if (slot >= 0) {
            out.emit(Op::PUSH_LOCAL, static_cast<std::uint8_t>(slot));
            return;
        }
        std::size_t idx = out.addSymbol(name);
        if (idx > 255) throw CompileError("const-pool overflow on symbol");
        out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(idx));
        return;
    }

    throw CompileError("compile: unsupported form in v0.0.x");
}

} // namespace protoClojure
