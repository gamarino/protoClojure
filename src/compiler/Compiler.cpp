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

// Session 9 — true iff `form` is a Reader-wrapped `[..]` vector literal.
// The underlying items live in a ProtoList under itemsKey.
static bool isWrappedVector(proto::ProtoContext* ctx,
                            const proto::ProtoObject* form,
                            const CompilerMarkers& markers) {
    if (!form) return false;
    const proto::ProtoObject* proto = form->getPrototype(ctx);
    return proto == markers.vectorMarkerProto;
}

// Returns the ProtoList of items under a wrapped vector. Caller has already
// verified isWrappedVector.
static const proto::ProtoList* vectorItems(proto::ProtoContext* ctx,
                                           const proto::ProtoObject* form,
                                           const CompilerMarkers& markers) {
    const proto::ProtoObject* raw =
        form->getAttribute(ctx, markers.itemsKey);
    return raw->asList(ctx);
}

// Session 13 — true iff `form` is a Reader-wrapped `{..}` map literal.
static bool isWrappedMap(proto::ProtoContext* ctx,
                         const proto::ProtoObject* form,
                         const CompilerMarkers& markers) {
    if (!form) return false;
    return form->getPrototype(ctx) == markers.mapMarkerProto;
}

static const proto::ProtoList* mapEntries(proto::ProtoContext* ctx,
                                          const proto::ProtoObject* form,
                                          const CompilerMarkers& markers) {
    const proto::ProtoObject* raw =
        form->getAttribute(ctx, markers.entriesKey);
    return raw->asList(ctx);
}

// Session 14 — true iff `form` is a stringy form whose UTF-8 starts
// with `:`. That is exactly a Clojure keyword as the Reader produces
// it (`:foo` → a Symbol whose first byte is `:`). Used to detect a
// trailing kv-pair suffix at call sites.
static bool isKeywordSymbol(proto::ProtoContext* ctx,
                            const proto::ProtoObject* form) {
    if (!isStringy(form)) return false;
    std::string s = asUtf8(ctx, form);
    return !s.empty() && s[0] == ':';
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
    const proto::ProtoList* params = nullptr;
    if (isWrappedVector(ctx, paramsForm, markers)) {
        params = vectorItems(ctx, paramsForm, markers);
    } else if (isList(paramsForm)) {
        // Pre-session-9 legacy: bare ProtoList counts as a params vector.
        // Multi-arity inner arities also reach here via compileArity's
        // direct call path; keep accepting raw lists for that.
        params = paramsForm->asList(ctx);
    } else {
        throw CompileError("fn/defn: parameter list must be a vector");
    }
    return compileArity(ctx, params, fnForm, paramsAt + 1, markers);
}

std::unique_ptr<BytecodeModule>
Compiler::compileArity(proto::ProtoContext* ctx,
                       const proto::ProtoList* params,
                       const proto::ProtoList* arityForm,
                       unsigned long bodyStartIdx,
                       const CompilerMarkers& markers) {
    unsigned long n = arityForm->getSize(ctx);
    unsigned long rawCount = params->getSize(ctx);

    // Session 7/8/13 — scan for `& <rest>`. The rest can be:
    //   - a symbol (`& rest`): variadic, collects leftover positionals as
    //     a list — the session-7 shape, unchanged.
    //   - a wrapped map (`& {:keys [k1 k2 ...]}`): kw-based, names each
    //     declared key as a local slot whose value will be supplied by
    //     the caller's kwArgs dict at CALL_KW time (session 13).
    //
    // The two shapes are mutually exclusive — JVM-Clojure shares the
    // surface (`& {:keys}` is the kw form of variadic), but the
    // dispatch path diverges entirely. We pick the path by inspecting
    // the form after `&`.
    unsigned long fixedArity = rawCount;
    bool isVariadic = false;
    bool isKwBased  = false;
    std::string restName;
    std::string asBindName;  // session 14 — `:as` snapshot binding name
    // Session 14 — each kw key may have an associated default form from
    // `:or {key default}`. defaultForm = nullptr when no default.
    struct KwKeyDecl {
        std::string name;
        const proto::ProtoObject* defaultForm;
    };
    std::vector<KwKeyDecl> kwKeyDecls;
    for (unsigned long i = 0; i < rawCount; ++i) {
        const proto::ProtoObject* p = params->getAt(ctx, (int)i);
        if (isStringy(p) && asUtf8(ctx, p) == "&") {
            if (i + 1 != rawCount - 1) {
                throw CompileError(
                    "fn/defn: `&` must be followed by exactly one form");
            }
            const proto::ProtoObject* rp = params->getAt(ctx, (int)(i + 1));
            fixedArity = i;

            if (isWrappedMap(ctx, rp, markers)) {
                // `& {:keys [k1 k2] :or {k1 default} :as opts}` — kw
                // destructuring.
                isKwBased = true;
                const proto::ProtoList* entries = mapEntries(ctx, rp, markers);
                unsigned long ne = entries->getSize(ctx);
                bool sawKeys = false;
                // Defer reading :or until after :keys — so we can attach
                // defaults to the right KwKeyDecl regardless of source
                // order. Cache the :or entries list pointer.
                const proto::ProtoList* orEntries = nullptr;
                for (unsigned long j = 0; j < ne; j += 2) {
                    const proto::ProtoObject* mk = entries->getAt(ctx, (int)j);
                    if (!isStringy(mk))
                        throw CompileError("fn/defn: `& {...}` key must be a keyword");
                    std::string mkName = asUtf8(ctx, mk);
                    const proto::ProtoObject* mv =
                        entries->getAt(ctx, (int)(j + 1));
                    if (mkName == ":keys") {
                        if (!isWrappedVector(ctx, mv, markers)) {
                            throw CompileError(
                                "fn/defn: `:keys` must be a vector of symbols");
                        }
                        const proto::ProtoList* ks = vectorItems(ctx, mv, markers);
                        unsigned long nk = ks->getSize(ctx);
                        for (unsigned long t = 0; t < nk; ++t) {
                            const proto::ProtoObject* sym = ks->getAt(ctx, (int)t);
                            if (!isStringy(sym))
                                throw CompileError("fn/defn: `:keys` entry must be a symbol");
                            kwKeyDecls.push_back({asUtf8(ctx, sym), nullptr});
                        }
                        sawKeys = true;
                    } else if (mkName == ":or") {
                        if (!isWrappedMap(ctx, mv, markers)) {
                            throw CompileError("fn/defn: `:or` must be a map");
                        }
                        orEntries = mapEntries(ctx, mv, markers);
                    } else if (mkName == ":as") {
                        if (!isStringy(mv)) {
                            throw CompileError("fn/defn: `:as` binding must be a symbol");
                        }
                        asBindName = asUtf8(ctx, mv);
                    } else {
                        throw CompileError(
                            std::string("fn/defn: unsupported entry in `& {...}`: ") + mkName);
                    }
                }
                if (!sawKeys) {
                    throw CompileError("fn/defn: `& {...}` needs at least :keys");
                }
                // Attach :or defaults to the matching KwKeyDecl.
                if (orEntries) {
                    unsigned long nor = orEntries->getSize(ctx);
                    for (unsigned long j = 0; j < nor; j += 2) {
                        const proto::ProtoObject* ok = orEntries->getAt(ctx, (int)j);
                        if (!isStringy(ok))
                            throw CompileError("fn/defn: `:or` key must be a symbol");
                        std::string okName = asUtf8(ctx, ok);
                        const proto::ProtoObject* ov =
                            orEntries->getAt(ctx, (int)(j + 1));
                        bool matched = false;
                        for (auto& d : kwKeyDecls) {
                            if (d.name == okName) {
                                d.defaultForm = ov;
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) {
                            throw CompileError(
                                "fn/defn: `:or` key not in :keys: " + okName);
                        }
                    }
                }
            } else if (isStringy(rp)) {
                // `& rest` — variadic, session-7 shape.
                restName   = asUtf8(ctx, rp);
                isVariadic = true;
            } else {
                throw CompileError(
                    "fn/defn: form after `&` must be a symbol (variadic) or a `{:keys [...]}` map");
            }
            break;
        }
    }

    auto body = std::make_unique<BytecodeModule>();
    body->setArity(static_cast<int>(fixedArity));
    body->setVariadic(isVariadic);
    body->setKwBased(isKwBased);

    // Push a fresh fn scope. Fixed params go in slots 0..fixedArity-1; the
    // rest-binding (if any) gets slot fixedArity. We never hold a Scope&
    // across a recursive compileForm() — that call may compile a nested
    // fn, push_back another Scope, and reallocate `scopes_`, which would
    // dangle the reference. Always re-acquire via scopes_.back().
    scopes_.push_back(Scope{});
    {
        Scope& scope = scopes_.back();
        scope.arity = static_cast<int>(fixedArity);
        for (unsigned long i = 0; i < fixedArity; ++i) {
            const proto::ProtoObject* p = params->getAt(ctx, (int)i);
            if (!isStringy(p)) {
                scopes_.pop_back();
                throw CompileError("fn/defn: parameter name must be a symbol");
            }
            scope.nameToSlot[asUtf8(ctx, p)] = static_cast<int>(i);
        }
        scope.nextSlot = static_cast<int>(fixedArity);
        if (isVariadic) {
            int restSlot = scope.nextSlot++;
            scope.nameToSlot[restName] = restSlot;
        }
        if (isKwBased) {
            for (const auto& d : kwKeyDecls) {
                int slot = scope.nextSlot++;
                scope.nameToSlot[d.name] = slot;
                body->addKwKey(d.name, slot);
            }
            if (!asBindName.empty()) {
                int slot = scope.nextSlot++;
                scope.nameToSlot[asBindName] = slot;
                body->setAsSlot(slot);
            }
        }
    }

    // Session 14 — `:or` default prologue. For each kw key with a
    // declared default, emit:
    //     PUSH_LOCAL <slot>
    //     PUSH_NIL
    //     EQ
    //     JUMP_IF_FALSE skip
    //     <compile default>
    //     STORE_LOCAL <slot>
    //     skip:
    // The slot was populated by the dispatcher (or left nil if the key
    // was missing). The `:or` default fires for either case; that
    // collapses the "missing" / "explicit nil" distinction, which is a
    // known v0.14 deviation from JVM-Clojure documented in STATUS.
    if (isKwBased) {
        for (std::size_t ki = 0; ki < kwKeyDecls.size(); ++ki) {
            if (!kwKeyDecls[ki].defaultForm) continue;
            int slot = body->kwKeys()[ki].localSlot;
            body->emit(Op::PUSH_LOCAL, static_cast<std::uint8_t>(slot));
            body->emit(Op::PUSH_NIL, 0);
            body->emit(Op::EQ, 0);
            std::size_t jifAt = body->emit(Op::JUMP_IF_FALSE, 0);
            compileForm(ctx, kwKeyDecls[ki].defaultForm, *body, markers);
            body->emit(Op::STORE_LOCAL, static_cast<std::uint8_t>(slot));
            std::size_t after = body->pos();
            std::size_t off = (after - (jifAt + kInstrSize)) / kInstrSize;
            if (off > 255)
                throw CompileError(":or default body too large");
            body->patchOperand(jifAt, static_cast<std::uint8_t>(off));
        }
    }

    // Session 8 — implicit recur target at the top of the body. recur
    // rebinds the fn's fixed params and jumps back to the body's first
    // instruction. (A variadic param could be supported too, but Clojure
    // semantics for recur over &rest require subtler handling; defer.)
    {
        Scope::RecurTarget tgt;
        tgt.bodyStart = body->pos();
        for (int i = 0; i < static_cast<int>(fixedArity); ++i) tgt.slots.push_back(i);
        scopes_.back().recurStack.push_back(tgt);
    }

    // Compile the body forms after the params vector.
    if (bodyStartIdx >= n) {
        // Empty body → return nil.
        body->emit(Op::PUSH_NIL, 0);
    } else {
        for (unsigned long i = bodyStartIdx; i < n; ++i) {
            compileForm(ctx, arityForm->getAt(ctx, (int)i), *body, markers);
            if (i + 1 < n) body->emit(Op::POP, 0);
        }
    }
    body->emit(Op::RETURN, 0);
    if (!scopes_.back().recurStack.empty()) {
        scopes_.back().recurStack.pop_back();
    }

    // Re-acquire — nested fn compilation may have grown scopes_ and
    // invalidated any reference held before the body loop.
    Scope& scope = scopes_.back();
    body->setLocalCount(scope.nextSlot - scope.arity);
    for (const auto& c : scope.captures) {
        body->addCapture(c.parentSlot, c.localSlot);
    }
    scopes_.pop_back();
    return body;
}

int Compiler::resolveLocal(const std::string& name) {
    if (scopes_.empty()) return -1;

    // Find the deepest scope at index `foundIdx` that has `name`.
    int top = static_cast<int>(scopes_.size()) - 1;
    int foundIdx = -1;
    for (int i = top; i >= 0; --i) {
        if (scopes_[i].nameToSlot.count(name)) {
            foundIdx = i;
            break;
        }
    }
    if (foundIdx < 0)      return -1;
    if (foundIdx == top)   return scopes_[top].nameToSlot[name];

    // Found in an outer scope. Walk back upward, creating a capture in
    // every intermediate scope that does not already host `name`. Each
    // intermediate capture sources its value from the immediately-
    // enclosing scope's slot for `name` (which we just created the
    // previous iteration if it wasn't already there).
    int sourceSlot = scopes_[foundIdx].nameToSlot[name];
    for (int i = foundIdx + 1; i <= top; ++i) {
        auto& s = scopes_[i];
        auto it = s.nameToSlot.find(name);
        if (it != s.nameToSlot.end()) {
            // Already a slot in this scope (a previous capture or a
            // genuine local) — adopt it as the source for the next level.
            sourceSlot = it->second;
            continue;
        }
        int newSlot = s.nextSlot++;
        if (newSlot > 255) {
            throw CompileError("closure: local-slot overflow (>255)");
        }
        s.nameToSlot[name] = newSlot;
        s.captures.push_back({sourceSlot, newSlot});
        sourceSlot = newSlot;
    }
    return scopes_[top].nameToSlot[name];
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

    // Float literal — emit PUSH_CONST <addDouble>.
    if (form->isFloat(ctx)) {
        std::size_t idx = out.addDouble(form->asDouble(ctx));
        if (idx > 255) {
            throw CompileError("const-pool overflow on float");
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
        // Special-form dispatch keys on the head's text; only symbols
        // qualify. A non-stringy head (e.g. `((outer 100) 20)` where the
        // callable is itself an expression) is always a regular call and
        // is compiled below via compileForm on the head.
        std::string headName = isStringy(head) ? asUtf8(ctx, head) : std::string{};

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

        // (when test body...) → desugar to (if test (do body...) nil).
        if (headName == "when" || headName == "when-not") {
            if (n < 2) throw CompileError("when/when-not: expects test + body");
            bool negate = (headName == "when-not");
            compileForm(ctx, lst->getAt(ctx, 1), out, markers);   // test
            std::size_t jifAt = out.emit(
                negate ? Op::JUMP_IF_TRUE : Op::JUMP_IF_FALSE, 0);
            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);
            } else {
                for (unsigned long i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }
            std::size_t jmpAt = out.emit(Op::JUMP, 0);
            std::size_t elseStart = out.pos();
            std::size_t elseOffset =
                (elseStart - (jifAt + kInstrSize)) / kInstrSize;
            if (elseOffset > 255) {
                throw CompileError("when: body too large for 1-byte offset");
            }
            out.patchOperand(jifAt, static_cast<std::uint8_t>(elseOffset));
            out.emit(Op::PUSH_NIL, 0);
            std::size_t after = out.pos();
            std::size_t jOff = (after - (jmpAt + kInstrSize)) / kInstrSize;
            if (jOff > 255) {
                throw CompileError("when: tail too large for 1-byte offset");
            }
            out.patchOperand(jmpAt, static_cast<std::uint8_t>(jOff));
            return;
        }

        // (cond test1 expr1 test2 expr2 ... [:else expr])
        //   Compiles to a chain of JUMP_IF_FALSE-skipping each pair.
        //   `:else` is a literal symbol that compiles to true (any truthy
        //   value works). If no clause matches and no :else is given,
        //   yields nil.
        if (headName == "cond") {
            unsigned long pn = n - 1;
            if (pn % 2 != 0) {
                throw CompileError("cond: clauses must come in pairs");
            }
            std::vector<std::size_t> endPatches;
            for (unsigned long i = 1; i < n; i += 2) {
                const proto::ProtoObject* test = lst->getAt(ctx, (int)i);
                const proto::ProtoObject* expr = lst->getAt(ctx, (int)(i + 1));
                bool isElse = isStringy(test) &&
                              (asUtf8(ctx, test) == ":else" ||
                               asUtf8(ctx, test) == "else");
                std::size_t skipAt = 0;
                if (!isElse) {
                    compileForm(ctx, test, out, markers);
                    skipAt = out.emit(Op::JUMP_IF_FALSE, 0);
                }
                compileForm(ctx, expr, out, markers);
                endPatches.push_back(out.emit(Op::JUMP, 0));
                if (!isElse) {
                    std::size_t after = out.pos();
                    std::size_t off =
                        (after - (skipAt + kInstrSize)) / kInstrSize;
                    if (off > 255) {
                        throw CompileError("cond: clause too large");
                    }
                    out.patchOperand(skipAt, static_cast<std::uint8_t>(off));
                }
            }
            // Fall-through (no clause matched) yields nil.
            out.emit(Op::PUSH_NIL, 0);
            std::size_t end = out.pos();
            for (std::size_t at : endPatches) {
                std::size_t off = (end - (at + kInstrSize)) / kInstrSize;
                if (off > 255) {
                    throw CompileError("cond: total length too large");
                }
                out.patchOperand(at, static_cast<std::uint8_t>(off));
            }
            return;
        }

        // (and a b c ...) — short-circuit. Each value is evaluated; the
        // result is the first falsey, or the last truthy. Implementation:
        //   compile a, DUP, JUMP_IF_FALSE end, POP, compile b, DUP, ...
        if (headName == "and") {
            if (n == 1) { out.emit(Op::PUSH_TRUE, 0); return; }
            std::vector<std::size_t> shortAts;
            for (unsigned long i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                if (i + 1 < n) {
                    out.emit(Op::DUP, 0);
                    shortAts.push_back(out.emit(Op::JUMP_IF_FALSE, 0));
                    out.emit(Op::POP, 0);  // drop the truthy copy left by DUP
                }
            }
            std::size_t end = out.pos();
            for (std::size_t at : shortAts) {
                std::size_t off = (end - (at + kInstrSize)) / kInstrSize;
                if (off > 255) throw CompileError("and: too large");
                out.patchOperand(at, static_cast<std::uint8_t>(off));
            }
            return;
        }

        // (or a b c ...) — short-circuit. Result is the first truthy, or
        // the last value. Mirror of and using JUMP_IF_TRUE.
        if (headName == "or") {
            if (n == 1) { out.emit(Op::PUSH_NIL, 0); return; }
            std::vector<std::size_t> shortAts;
            for (unsigned long i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                if (i + 1 < n) {
                    out.emit(Op::DUP, 0);
                    shortAts.push_back(out.emit(Op::JUMP_IF_TRUE, 0));
                    out.emit(Op::POP, 0);
                }
            }
            std::size_t end = out.pos();
            for (std::size_t at : shortAts) {
                std::size_t off = (end - (at + kInstrSize)) / kInstrSize;
                if (off > 255) throw CompileError("or: too large");
                out.patchOperand(at, static_cast<std::uint8_t>(off));
            }
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

        // (fn ...) / (defn name ...) — single OR multi-arity. Multi-arity
        // is detected by inspecting the form immediately after the head
        // (and the name, for defn): if it is a list whose first element
        // is itself a list, the form is multi-arity. Each subsequent form
        // is then a `(params body...)` arity body.
        if (headName == "fn" || headName == "defn") {
            unsigned long aritiesStart = (headName == "fn") ? 1 : 2;
            if (n <= aritiesStart) {
                throw CompileError("fn/defn: missing parameter vector");
            }
            const proto::ProtoObject* nameForm = nullptr;
            if (headName == "defn") {
                nameForm = lst->getAt(ctx, 1);
                if (!isStringy(nameForm)) {
                    throw CompileError("defn: name must be a symbol");
                }
            }

            const proto::ProtoObject* first = lst->getAt(ctx, (int)aritiesStart);
            // Single-arity: the form right after the head/name is a vector
            // literal `[..]` (wrapped). Multi-arity: a list whose first
            // element is itself a wrapped vector — `(fn ([x] ...) ([x y] ...))`.
            bool multiArity = false;
            if (isList(first)) {
                const proto::ProtoList* fl = first->asList(ctx);
                if (fl->getSize(ctx) >= 1 &&
                    isWrappedVector(ctx, fl->getAt(ctx, 0), markers)) {
                    multiArity = true;
                }
            }

            auto emitCapsThenOp = [&](std::size_t blockIdx, Op makeOp,
                                      std::uint8_t makeOperand) {
                const auto& caps = out.block(blockIdx).captureSpecs();
                for (const auto& c : caps) {
                    if (c.parentSlot < 0 || c.parentSlot > 255) {
                        throw CompileError("fn: capture parent-slot overflow");
                    }
                    out.emit(Op::PUSH_LOCAL,
                        static_cast<std::uint8_t>(c.parentSlot));
                }
                out.emit(makeOp, makeOperand);
            };

            if (!multiArity) {
                // Single-arity — unchanged from session 6.
                std::unique_ptr<BytecodeModule> body =
                    compileFnBody(ctx, lst, markers);
                std::size_t blockIdx = out.addBlock(std::move(body));
                if (blockIdx > 255) {
                    throw CompileError("fn: too many fn bodies (>255) in module");
                }
                emitCapsThenOp(blockIdx, Op::MAKE_FN,
                               static_cast<std::uint8_t>(blockIdx));
            } else {
                // Multi-arity — compile each `(params body...)` separately,
                // push their captures (arity 0 first), emit MAKE_FN_MULTI.
                std::vector<std::size_t> blockIdxs;
                for (unsigned long i = aritiesStart; i < n; ++i) {
                    const proto::ProtoObject* aobj = lst->getAt(ctx, (int)i);
                    if (!isList(aobj)) {
                        throw CompileError("multi-arity: each arity must be a list");
                    }
                    const proto::ProtoList* alist = aobj->asList(ctx);
                    if (alist->getSize(ctx) < 1) {
                        throw CompileError("multi-arity: arity missing params");
                    }
                    const proto::ProtoObject* paramsForm = alist->getAt(ctx, 0);
                    const proto::ProtoList* params = nullptr;
                    if (isWrappedVector(ctx, paramsForm, markers)) {
                        params = vectorItems(ctx, paramsForm, markers);
                    } else if (isList(paramsForm)) {
                        params = paramsForm->asList(ctx);
                    } else {
                        throw CompileError("multi-arity: params must be a vector");
                    }
                    std::unique_ptr<BytecodeModule> body =
                        compileArity(ctx, params, alist, 1, markers);
                    std::size_t blockIdx = out.addBlock(std::move(body));
                    if (blockIdx > 255) {
                        throw CompileError("fn: too many fn bodies (>255)");
                    }
                    blockIdxs.push_back(blockIdx);
                }
                // Push captures in arity-emission order so the VM can pop
                // them back into per-arity lists.
                for (std::size_t bi : blockIdxs) {
                    const auto& caps = out.block(bi).captureSpecs();
                    for (const auto& c : caps) {
                        if (c.parentSlot < 0 || c.parentSlot > 255) {
                            throw CompileError("fn: capture parent-slot overflow");
                        }
                        out.emit(Op::PUSH_LOCAL,
                            static_cast<std::uint8_t>(c.parentSlot));
                    }
                }
                std::size_t groupIdx = out.addArityGroup(std::move(blockIdxs));
                if (groupIdx > 255) {
                    throw CompileError("fn: too many arity groups (>255)");
                }
                out.emit(Op::MAKE_FN_MULTI,
                         static_cast<std::uint8_t>(groupIdx));
            }

            if (headName == "defn") {
                std::size_t nameIdx = out.addSymbol(asUtf8(ctx, nameForm));
                if (nameIdx > 255) throw CompileError("defn: const-pool overflow");
                out.emit(Op::STORE_GLOBAL, static_cast<std::uint8_t>(nameIdx));
            }
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
            const proto::ProtoList* bvec = nullptr;
            if (isWrappedVector(ctx, bindings, markers)) {
                bvec = vectorItems(ctx, bindings, markers);
            } else if (isList(bindings)) {
                bvec = bindings->asList(ctx);
            } else {
                throw CompileError("let: bindings must be a vector");
            }
            unsigned long bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("let: bindings must come in name/value pairs");
            }
            for (unsigned long i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isStringy(nameForm)) {
                    throw CompileError("let: binding name must be a symbol");
                }
                compileForm(ctx, valForm, out, markers);  // push value
                // Re-acquire scope each iteration: compileForm may have
                // pushed nested fn scopes and reallocated scopes_.
                Scope& scope = scopes_.back();
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
            const proto::ProtoList* bvec = nullptr;
            if (isWrappedVector(ctx, bindings, markers)) {
                bvec = vectorItems(ctx, bindings, markers);
            } else if (isList(bindings)) {
                bvec = bindings->asList(ctx);
            } else {
                throw CompileError("loop: bindings must be a vector");
            }
            unsigned long bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("loop: bindings must come in name/value pairs");
            }
            std::vector<int> recurSlots;
            for (unsigned long i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isStringy(nameForm)) {
                    throw CompileError("loop: binding name must be a symbol");
                }
                compileForm(ctx, valForm, out, markers);
                Scope& scope = scopes_.back();    // re-acquire post-recursion
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
            scopes_.back().recurStack.push_back(tgt);

            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);
            } else {
                for (unsigned long i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }

            scopes_.back().recurStack.pop_back();
            return;
        }

        // (apply f args-list) — special form. Compile f, compile the
        // args-list (any expression that yields a list), emit CALL_APPLY.
        // v0.7.x supports only the two-arg shape; the JVM-Clojure variadic
        // form `(apply f x y coll)` lands once multi-arity arrives.
        if (headName == "apply") {
            if (n != 3) {
                throw CompileError(
                    "apply: v0.7.x supports only (apply f args-list)");
            }
            compileForm(ctx, lst->getAt(ctx, 1), out, markers);
            compileForm(ctx, lst->getAt(ctx, 2), out, markers);
            out.emit(Op::CALL_APPLY, 0);
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

        // Session 11 — SmallInt fast-path emission. For the standard
        // operator names AND when the symbol is NOT shadowed by a local
        // binding, we emit binary-fold opcodes (ADD / SUB / MUL / LT /
        // LE / GT / GE / EQ) directly. The VM short-circuits when both
        // operands are tagged SmallInt; otherwise it falls back to the
        // primitive on globals — same semantics as PUSH_VAR + CALL.
        if (isStringy(head)) {
            Op binOp = Op::NOP;
            if      (headName == "+")  binOp = Op::ADD;
            else if (headName == "-")  binOp = Op::SUB;
            else if (headName == "*")  binOp = Op::MUL;
            else if (headName == "<")  binOp = Op::LT;
            else if (headName == "<=") binOp = Op::LE;
            else if (headName == ">")  binOp = Op::GT;
            else if (headName == ">=") binOp = Op::GE;
            else if (headName == "=")  binOp = Op::EQ;

            // Shadow check — if `+` (or whichever) is bound locally, we
            // are calling the local binding, not the global primitive;
            // fall back to the general PUSH_VAR + CALL path.
            int shadowSlot = (binOp != Op::NOP) ? resolveLocal(headName) : -1;
            unsigned long argc = n - 1;

            if (binOp != Op::NOP && shadowSlot < 0 && argc >= 2) {
                // Arithmetic: left-fold pairs. (op a b c d) emits
                // a; b; OP; c; OP; d; OP. Comparisons fold the same way
                // semantically — (< a b c) becomes ((< a b) && (< b c))
                // in Clojure, but our SmallInt fast-path opcodes are
                // strictly binary; for >2 args on comparisons, fall
                // through to the global primitive (correct semantics).
                bool isCompare = (binOp == Op::LT || binOp == Op::LE ||
                                  binOp == Op::GT || binOp == Op::GE ||
                                  binOp == Op::EQ);
                if (isCompare && argc != 2) {
                    // Defer to general path for chained comparisons.
                } else {
                    compileForm(ctx, lst->getAt(ctx, 1), out, markers);
                    for (unsigned long i = 2; i < n; ++i) {
                        compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                        out.emit(binOp, 0);
                    }
                    return;
                }
            }

            // General path — emit callable, then args, then CALL.
            int slot = resolveLocal(headName);
            if (slot >= 0) {
                out.emit(Op::PUSH_LOCAL, static_cast<std::uint8_t>(slot));
            } else {
                std::size_t headIdx = out.addSymbol(headName);
                if (headIdx > 255) {
                    throw CompileError("const-pool overflow on symbol");
                }
                out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(headIdx));
            }
        } else {
            compileForm(ctx, head, out, markers);
        }

        // Session 14 — trailing kv-pair detection. Walking backwards
        // from the last arg in steps of 2, every position that should
        // host a key must be a keyword. The longest such suffix is
        // packaged into a synthetic `(hash-map :k v :k v ...)` call and
        // emitted as the LAST positional argument; the callee can then
        // peel it off when isKwBased (session 13's path).
        unsigned long kvStart = n;
        for (long long i = (long long)n - 2; i >= 1; i -= 2) {
            const proto::ProtoObject* arg =
                lst->getAt(ctx, static_cast<int>(i));
            if (isKeywordSymbol(ctx, arg)) {
                kvStart = static_cast<unsigned long>(i);
            } else {
                break;
            }
        }
        unsigned long posCount = kvStart - 1;
        unsigned long kvCount  = n - kvStart;

        // Push positionals.
        for (unsigned long i = 1; i < kvStart; ++i) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
        }

        // If a kv suffix was detected, build the kw map at runtime by
        // calling `hash-map` with all kv entries; the result becomes
        // the extra trailing positional arg.
        if (kvCount > 0) {
            std::size_t hmIdx = out.addSymbol("hash-map");
            if (hmIdx > 255) throw CompileError("hash-map: const-pool overflow");
            out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(hmIdx));
            for (unsigned long i = kvStart; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
            }
            if (kvCount > 255) {
                throw CompileError("kw suffix: >255 args not supported");
            }
            out.emit(Op::CALL, static_cast<std::uint8_t>(kvCount));
        }

        unsigned long callArgc = posCount + (kvCount > 0 ? 1 : 0);
        if (callArgc > 255) {
            throw CompileError("call with >255 args not supported");
        }
        // CALL_KW when a kv suffix was packaged; CALL otherwise. The VM
        // decides at runtime whether to keep the kwMap intact (kw-based
        // callee) or unpack it back into positional k,v,k,v args
        // (everyone else).
        out.emit(kvCount > 0 ? Op::CALL_KW : Op::CALL,
                 static_cast<std::uint8_t>(callArgc));
        return;
    }

    // Session 13 — `{..}` map literal in expression position. Compile as
    // a call to the `hash-map` primitive: emit PUSH_VAR hash-map, then
    // each k,v in source order, then CALL 2*N. The wrapper marker stays
    // out of the bytecode — purely a compile-time hint.
    if (isWrappedMap(ctx, form, markers)) {
        const proto::ProtoList* entries = mapEntries(ctx, form, markers);
        unsigned long ne = entries->getSize(ctx);  // already validated even
        std::size_t headIdx = out.addSymbol("hash-map");
        if (headIdx > 255) throw CompileError("hash-map: const-pool overflow");
        out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(headIdx));
        for (unsigned long i = 0; i < ne; ++i) {
            compileForm(ctx, entries->getAt(ctx, (int)i), out, markers);
        }
        if (ne > 255) throw CompileError("hash-map: >255 items in literal");
        out.emit(Op::CALL, static_cast<std::uint8_t>(ne));
        return;
    }

    // Session 9 — `[..]` vector literal in expression position. Compile as
    // a call to the `vector` primitive: emit PUSH_VAR vector, then each
    // item, then CALL N. The wrapper marker stays out of the bytecode —
    // it is purely a compile-time hint to distinguish `[..]` from `(..)`.
    if (isWrappedVector(ctx, form, markers)) {
        const proto::ProtoList* items = vectorItems(ctx, form, markers);
        unsigned long ni = items->getSize(ctx);
        std::size_t headIdx = out.addSymbol("vector");
        if (headIdx > 255) throw CompileError("vector: const-pool overflow");
        out.emit(Op::PUSH_VAR, static_cast<std::uint8_t>(headIdx));
        for (unsigned long i = 0; i < ni; ++i) {
            compileForm(ctx, items->getAt(ctx, (int)i), out, markers);
        }
        if (ni > 255) throw CompileError("vector: >255 items in literal");
        out.emit(Op::CALL, static_cast<std::uint8_t>(ni));
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

    // Bare stringy atom — keyword literal, reserved literal (true / false
    // / nil), local reference, or runtime global lookup, in that order.
    if (isStringy(form)) {
        std::string name = asUtf8(ctx, form);
        if (name == "true")  { out.emit(Op::PUSH_TRUE,  0); return; }
        if (name == "false") { out.emit(Op::PUSH_FALSE, 0); return; }
        if (name == "nil")   { out.emit(Op::PUSH_NIL,   0); return; }
        if (!name.empty() && name[0] == ':') {
            std::size_t idx = out.addSymbol(name);
            if (idx > 255) throw CompileError("const-pool overflow on keyword");
            out.emit(Op::PUSH_CONST, static_cast<std::uint8_t>(idx));
            return;
        }
        int slot = resolveLocal(name);
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
