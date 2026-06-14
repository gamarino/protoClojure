/*
 * BytecodeModule — a compiled body of code: a const pool + an instruction
 * byte vector.
 *
 * P3 note: BytecodeModule holds std::vectors internally. This is OK
 * because BytecodeModule is a C++-side type owned by the runtime; no
 * ProtoObject* ever references a BytecodeModule directly. The runtime
 * owns BytecodeModules via std::unique_ptr; protoCore is unaware they
 * exist. When the runtime tears down, the modules destruct cleanly with
 * their std internals. No mixing.
 *
 * If session 4+ needs to attach a BytecodeModule to a ProtoObject (for
 * a closure, for example), the attachment is via an opaque
 * ProtoExternalPointer with a finalizer that calls `delete`. P4 — record
 * the boundary explicitly when we make that move.
 */
#pragma once
#include "Opcodes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace protoClojure {

class BytecodeModule {
public:
    enum class ConstKind : uint8_t {
        Long,
        String,
        Symbol,    // interned at materialisation time via ProtoString::createSymbol
    };

    struct Const {
        ConstKind kind;
        long long ival = 0;
        std::string sval;        // only the std-side raw bytes; never enters protoCore
    };

    // Const-pool insertion. Returns the index for use in PUSH_CONST / PUSH_VAR.
    std::size_t addLong(long long v);
    std::size_t addString(const std::string& s);
    std::size_t addSymbol(const std::string& s);

    // Emit one instruction word (opcode + operand). The operand must fit in
    // one byte for v0.0.x; the EXTEND-prefix mechanism is a session-5 follow-up.
    // Returns the byte offset of the *opcode* byte — used by patchOperand to
    // back-patch JUMP / JUMP_IF_FALSE forward-targets.
    std::size_t emit(Op op, std::uint8_t operand);

    // Rewrite the operand byte of an already-emitted instruction. Used to
    // back-patch forward jumps once the target PC is known.
    void patchOperand(std::size_t opcodeOffset, std::uint8_t newOperand);

    // The current byte position — used as the "now" point for computing a
    // forward jump's offset.
    std::size_t pos() const { return bytes_.size(); }

    // Read-only access for the executor.
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    const Const& constAt(std::size_t i) const { return consts_[i]; }
    std::size_t constCount() const { return consts_.size(); }

    // Function metadata used by user-fn dispatch in the VM.
    int  arity()        const { return arity_; }
    int  localCount()   const { return localCount_; }
    bool isVariadic()   const { return isVariadic_; }
    void setArity(int n)        { arity_ = n; }
    void setLocalCount(int n)   { localCount_ = n; }
    void setVariadic(bool v)    { isVariadic_ = v; }

    // Closure capture specification (session 6). For each free variable
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
    std::vector<std::uint8_t>                    bytes_;
    std::vector<Const>                           consts_;
    std::vector<std::unique_ptr<BytecodeModule>> blocks_;
    std::vector<ArityGroup>                      arityGroups_;
    std::vector<CaptureSpec>                     captureSpecs_;
    int                                          arity_      = 0;
    int                                          localCount_ = 0;
    bool                                         isVariadic_ = false;
};

} // namespace protoClojure
