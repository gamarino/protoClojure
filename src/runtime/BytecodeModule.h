/*
 * BytecodeModule — a compiled body of code: a const pool + a vector of
 * instruction words (Opcodes.h).
 *
 * The const pool holds each constant once: adding a constant equal to an
 * existing one of the same kind returns the existing index (hash-indexed,
 * O(1) per add). Kinds never merge — 1 and 1.0, the string "a", the keyword
 * :a and the global name a are distinct entries — and doubles are compared
 * by bit pattern, so 0.0 and -0.0 stay distinct. A fn body is its own
 * module with its own pool; the top level of a script or REPL form is one
 * module.
 *
 * P3 note: BytecodeModule holds std::vectors internally. This is OK
 * because BytecodeModule is a C++-side type owned by the runtime; no
 * ProtoObject* ever references a BytecodeModule directly. The runtime
 * owns BytecodeModules via std::unique_ptr; protoCore is unaware they
 * exist. When the runtime tears down, the modules destruct cleanly with
 * their std internals. No mixing.
 *
 * If later code needs to attach a BytecodeModule to a ProtoObject (for
 * a closure, for example), the attachment is via an opaque
 * ProtoExternalPointer with a finalizer that calls `delete`. P4 — record
 * the boundary explicitly when we make that move.
 *
 * P4 boundary: interned keyword and symbol values (Const::named,
 * KwKey::keyword) are the one kind of ProtoObject* a module holds. The
 * compiler interns them (src/runtime/Named.h), and they stay reachable
 * through the runtime's rooted intern table for the lifetime of the
 * ProtoSpace, so the GC needs no reference from here. A module therefore
 * runs only in the ProtoSpace it was compiled against.
 */
#pragma once
#include "Opcodes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace proto {
class ProtoObject;
}

namespace protoClojure {

class BytecodeModule {
public:
    enum class ConstKind : uint8_t {
        Long,
        Double,    // IEEE-754 64-bit float literal
        String,
        Symbol,    // a global name for PUSH_VAR / STORE_GLOBAL, interned at
                   // execution time via ProtoString::createSymbol
        Named,     // a keyword or quoted-symbol value for PUSH_CONST,
                   // interned at compile time (Named.h)
        BigInteger,  // an integer beyond the long long range, kept as its
                     // decimal digits in `sval`; PUSH_CONST rebuilds the
                     // LargeInteger on each execution (a module holds no
                     // unrooted heap values)
    };

    struct Const {
        ConstKind kind;
        long long ival = 0;
        double    dval = 0.0;
        std::string sval;        // only the std-side raw bytes; never enters protoCore
        const proto::ProtoObject* named = nullptr;  // Named: the interned value
    };

    // Const-pool insertion. Returns the index for use in PUSH_CONST / PUSH_VAR;
    // a constant equal to an existing one of the same kind reuses its index.
    std::size_t addLong(long long v);
    // An integer beyond the long long range, as decimal digits ("-123...").
    std::size_t addBigInteger(const std::string& digits);
    std::size_t addDouble(double v);
    std::size_t addString(const std::string& s);
    std::size_t addSymbol(const std::string& s);
    // A keyword or quoted-symbol value: `value` is internNamed(spelling).
    // De-duplicated by spelling: interning maps a spelling to one value.
    std::size_t addNamed(const std::string& spelling,
                         const proto::ProtoObject* value);

    // Emit one instruction word. Returns its position, which patchOperand
    // uses to back-patch a forward jump once the target is known. Throws
    // std::length_error when `operand` exceeds kMaxOperand.
    std::size_t emit(Op op, std::size_t operand);

    // Rewrite the operand of the instruction at `at`. Throws
    // std::length_error when `operand` exceeds kMaxOperand and
    // std::out_of_range when `at` is not an emitted instruction.
    void patchOperand(std::size_t at, std::size_t operand);

    // The position of the next instruction — the "now" point for computing
    // a jump's offset, in instruction words.
    std::size_t pos() const { return code_.size(); }

    // Read-only access for the executor.
    const std::vector<Instr>& code() const { return code_; }
    const Const& constAt(std::size_t i) const { return consts_[i]; }
    std::size_t constCount() const { return consts_.size(); }

    // Function metadata used by user-fn dispatch in the VM.
    int  arity()        const { return arity_; }
    int  localCount()   const { return localCount_; }
    bool isVariadic()   const { return isVariadic_; }
    void setArity(int n)        { arity_ = n; }
    void setLocalCount(int n)   { localCount_ = n; }
    void setVariadic(bool v)    { isVariadic_ = v; }

    // Session 13 — named-arg destructuring. When a fn body is
    // declared with `& {:keys [k1 k2 ...]}`, each key gets a local
    // slot and the dispatcher reads it from the kwArgs dict at call
    // time. The slot indices are stored parallel to the names so the
    // VM can populate them in one pass. `isKwBased()` short-circuits
    // the rest-arg path for kw fns.
    struct KwKey {
        std::string               name;
        int                       localSlot;
        const proto::ProtoObject* keyword;  // the interned keyword `:name`
    };
    bool isKwBased() const { return isKwBased_; }
    void setKwBased(bool v) { isKwBased_ = v; }
    void addKwKey(const std::string& name, int localSlot,
                  const proto::ProtoObject* keyword) {
        kwKeys_.push_back({name, localSlot, keyword});
    }
    const std::vector<KwKey>& kwKeys() const { return kwKeys_; }

    // Session 14 — `:as` binding slot (-1 when not declared).
    int  asSlot() const { return asSlot_; }
    void setAsSlot(int s) { asSlot_ = s; }

    // Closure capture specification. For each free variable
    // the body references through its enclosing scope, the compiler
    // records: parentSlot — the slot in the enclosing scope's frame to
    // read at MAKE_FN time; localSlot — the slot in THIS body's frame to
    // populate at CALL time from the wrapper's __captures__ list. Order
    // of captureSpecs matters: the outer-scope PUSH_LOCALs and the
    // wrapper's captures list both walk this order.
    struct CaptureSpec {
        int parentSlot;
        int localSlot;
    };
    void addCapture(int parentSlot, int localSlot) {
        captureSpecs_.push_back({parentSlot, localSlot});
    }
    const std::vector<CaptureSpec>& captureSpecs() const { return captureSpecs_; }
    int captureCount() const { return static_cast<int>(captureSpecs_.size()); }

    // Sub-module ownership for fn bodies. The compiler calls addBlock to
    // append a freshly-compiled fn body; MAKE_FN's operand is the returned
    // index. The C++-side std::unique_ptr ownership is invisible to
    // protoCore — the fn-wrapper holds an opaque pointer cast (see
    // ExecutionEngine MAKE_FN handler). P3 stays clean: protoCore does not
    // traverse into the std::vector.
    std::size_t addBlock(std::unique_ptr<BytecodeModule> sub);
    const BytecodeModule& block(std::size_t i) const { return *blocks_[i]; }
    std::size_t blockCount() const { return blocks_.size(); }

    // Session 8 — multi-arity fn dispatch group. Each ArityGroup records
    // the block indices that make up the N arities of a single multi-
    // arity fn. The compiler emits MAKE_FN_MULTI with operand = the
    // group's index; the VM walks blockIndices in order to pop captures
    // and build the wrapper's __arities__ list.
    struct ArityGroup {
        std::vector<std::size_t> blockIndices;
    };
    std::size_t addArityGroup(std::vector<std::size_t> blockIndices) {
        arityGroups_.push_back(ArityGroup{std::move(blockIndices)});
        return arityGroups_.size() - 1;
    }
    const ArityGroup& arityGroup(std::size_t i) const { return arityGroups_[i]; }
    std::size_t arityGroupCount() const { return arityGroups_.size(); }

private:
    std::vector<Instr>                           code_;
    std::vector<Const>                           consts_;
    // Const-pool indices by value, one index per kind (doubles by bit pattern).
    std::unordered_map<long long, std::size_t>     longIndex_;
    std::unordered_map<std::uint64_t, std::size_t> doubleIndex_;
    std::unordered_map<std::string, std::size_t>   bigIntegerIndex_;
    std::unordered_map<std::string, std::size_t>   stringIndex_;
    std::unordered_map<std::string, std::size_t>   symbolIndex_;
    std::unordered_map<std::string, std::size_t>   namedIndex_;
    std::vector<std::unique_ptr<BytecodeModule>> blocks_;
    std::vector<ArityGroup>                      arityGroups_;
    std::vector<CaptureSpec>                     captureSpecs_;
    int                                          arity_      = 0;
    int                                          localCount_ = 0;
    bool                                         isVariadic_ = false;
    bool                                         isKwBased_  = false;
    int                                          asSlot_     = -1;
    std::vector<KwKey>                           kwKeys_;
};

} // namespace protoClojure
