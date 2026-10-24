#include "BytecodeModule.h"
#include "Named.h"
#include "Primitives.h"
#include "protoCore.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace protoClojure {

const char* opName(Op op) {
    switch (op) {
        case Op::NOP:           return "NOP";
        case Op::PUSH_CONST:    return "PUSH_CONST";
        case Op::PUSH_VAR:      return "PUSH_VAR";
        case Op::CALL:          return "CALL";
        case Op::POP:           return "POP";
        case Op::RETURN:        return "RETURN";
        case Op::STORE_GLOBAL:  return "STORE_GLOBAL";
        case Op::JUMP:          return "JUMP";
        case Op::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case Op::PUSH_NIL:      return "PUSH_NIL";
        case Op::PUSH_TRUE:     return "PUSH_TRUE";
        case Op::PUSH_FALSE:    return "PUSH_FALSE";
        case Op::PUSH_LOCAL:    return "PUSH_LOCAL";
        case Op::PUSH_LOCAL_CLEAR: return "PUSH_LOCAL_CLEAR";
        case Op::STORE_LOCAL:   return "STORE_LOCAL";
        case Op::MAKE_FN:       return "MAKE_FN";
        case Op::JUMP_BACK:     return "JUMP_BACK";
        case Op::CALL_APPLY:    return "CALL_APPLY";
        case Op::DUP:           return "DUP";
        case Op::JUMP_IF_TRUE:  return "JUMP_IF_TRUE";
        case Op::MAKE_FN_MULTI: return "MAKE_FN_MULTI";
        case Op::ADD:           return "ADD";
        case Op::SUB:           return "SUB";
        case Op::MUL:           return "MUL";
        case Op::LT:            return "LT";
        case Op::LE:            return "LE";
        case Op::GT:            return "GT";
        case Op::GE:            return "GE";
        case Op::EQ:            return "EQ";
        case Op::CALL_KW:       return "CALL_KW";
        case Op::TRY_BEGIN:     return "TRY_BEGIN";
        case Op::TRY_END:       return "TRY_END";
        case Op::THROW:         return "THROW";
        case Op::EXC_MATCH:     return "EXC_MATCH";
        case Op::RETHROW:       return "RETHROW";
        case Op::EXC_MATCH_TYPE: return "EXC_MATCH_TYPE";
        case Op::RESOLVE_CLASS: return "RESOLVE_CLASS";
        case Op::GET_MEMBER:    return "GET_MEMBER";
        case Op::CALL_MEMBER:   return "CALL_MEMBER";
        case Op::STORE_CAUGHT:  return "STORE_CAUGHT";
        case Op::LINE:          return "LINE";
    }
    return "?";
}

std::size_t BytecodeModule::addBlock(std::unique_ptr<BytecodeModule> sub) {
    blocks_.push_back(std::move(sub));
    return blocks_.size() - 1;
}

namespace {

// The index of the constant `key` names in `index`, appending `c` to the
// pool first when the key is new.
template <typename Key>
std::size_t findOrAdd(std::unordered_map<Key, std::size_t>& index,
                      const Key& key,
                      std::vector<BytecodeModule::Const>& consts,
                      BytecodeModule::Const c) {
    const auto [it, inserted] = index.try_emplace(key, consts.size());
    if (inserted) consts.push_back(std::move(c));
    return it->second;
}

std::uint32_t checkedOperand(Op op, std::size_t operand) {
    if (operand > kMaxOperand) {
        throw std::length_error(
            std::string(opName(op)) + " operand " + std::to_string(operand) +
            " exceeds the bytecode limit of " + std::to_string(kMaxOperand) +
            " (constants, locals, functions, arguments or instructions "
            "jumped over in one function body)");
    }
    return static_cast<std::uint32_t>(operand);
}

} // namespace

std::size_t BytecodeModule::addLong(long long v) {
    return findOrAdd(longIndex_, v, consts_, Const{ConstKind::Long, v, 0.0, {}});
}

std::size_t BytecodeModule::addBigInteger(const std::string& digits) {
    return findOrAdd(bigIntegerIndex_, digits, consts_,
                     Const{ConstKind::BigInteger, 0, 0.0, digits});
}

std::size_t BytecodeModule::addDouble(double v) {
    // By bit pattern: 0.0 and -0.0 are two constants, and a NaN literal
    // reuses the entry of an identical NaN.
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return findOrAdd(doubleIndex_, bits, consts_,
                     Const{ConstKind::Double, 0, v, {}});
}

std::size_t BytecodeModule::addString(const std::string& s) {
    return findOrAdd(stringIndex_, s, consts_, Const{ConstKind::String, 0, 0.0, s});
}

std::size_t BytecodeModule::addSymbol(proto::ProtoContext* ctx, const std::string& s) {
    // Interned once here, not on every PUSH_VAR / STORE_GLOBAL execution;
    // a symbol this module already holds is not interned again, and the
    // run's per-thread symbol cache (Named.h) serves the others.
    const auto known = symbolIndex_.find(s);
    if (known != symbolIndex_.end()) return known->second;
    const ActiveCallContext* cc = activeCallContext();
    const auto* key = reinterpret_cast<const proto::ProtoObject*>(
        cc ? internSymbol(ctx, cc->named, s) : proto::ProtoString::createSymbol(ctx, s.c_str()));
    return findOrAdd(symbolIndex_, s, consts_, Const{ConstKind::Symbol, 0, 0.0, s, key});
}

std::size_t BytecodeModule::addNamed(const std::string& spelling,
                                     const proto::ProtoObject* value) {
    return findOrAdd(namedIndex_, spelling, consts_,
                     Const{ConstKind::Named, 0, 0.0, spelling, value});
}

std::size_t BytecodeModule::addDatum(proto::ProtoContext* ctx,
                                     const proto::ProtoObject* value) {
    const auto it = datumIndex_.find(value);
    if (it != datumIndex_.end()) return it->second;
    if (!datumRoots_) {
        datumSpace_ = ctx->space;
        datumRoots_ = datumSpace_->createRootSet("protoclj-quoted-data");
    }
    datumRoots_->add(value);
    const std::size_t idx = consts_.size();
    consts_.push_back(Const{ConstKind::Datum, 0, 0.0, {}, value});
    datumIndex_.emplace(value, idx);
    return idx;
}

BytecodeModule::~BytecodeModule() {
    if (datumRoots_) datumSpace_->destroyRootSet(datumRoots_);
}

std::size_t BytecodeModule::emit(Op op, std::size_t operand) {
    const std::uint32_t checked = checkedOperand(op, operand);
    code_.push_back((checked << kOperandShift) | static_cast<Instr>(op));
    if (op == Op::TRY_BEGIN) hasHandlers_ = true;
    return code_.size() - 1;
}

void BytecodeModule::patchOperand(std::size_t at, std::size_t operand) {
    if (at >= code_.size()) {
        throw std::out_of_range("patchOperand: no instruction at " +
                                std::to_string(at));
    }
    const Op op = static_cast<Op>(code_[at] & 0xFF);
    code_[at] = (checkedOperand(op, operand) << kOperandShift) |
                static_cast<Instr>(op);
}

// The operand stack's depth before every instruction, by a forward flow
// analysis over the finished code; the answer is the deepest point.  Where
// two paths meet with different depths the larger one is kept, so the bound
// stays an upper bound.  Each instruction's effect mirrors
// ExecutionEngine::executeFrame:
//   +1  PUSH_CONST PUSH_VAR PUSH_NIL PUSH_TRUE PUSH_FALSE PUSH_LOCAL
//       PUSH_LOCAL_CLEAR DUP
//       RESOLVE_CLASS
//   -1  POP STORE_LOCAL STORE_CAUGHT JUMP_IF_FALSE JUMP_IF_TRUE, ADD .. EQ (two in, one out)
//       EXC_MATCH_TYPE
//    0  NOP STORE_GLOBAL JUMP JUMP_BACK TRY_END EXC_MATCH GET_MEMBER LINE
//  -argc CALL CALL_KW CALL_MEMBER (callable or receiver and argc arguments
//       in, the result out; CALL_MEMBER's data word is a NOP)
//   -1  CALL_APPLY (callable and list in, the result out; the spread
//       arguments are pushed at run time and are not counted)
//  1-caps MAKE_FN (its block's captures), MAKE_FN_MULTI (all its arities')
//   TRY_BEGIN: the handler starts one deeper (the exception is pushed)
//   RETURN, THROW, RETHROW: no successor.
// A jump's target is the next instruction plus (or, for JUMP_BACK, minus)
// its operand.
unsigned int BytecodeModule::computeMaxStack() const {
    const std::size_t n = code_.size();
    std::vector<int> depth(n, -1);
    std::vector<std::size_t> work;
    int deepest = 0;
    auto reach = [&](std::size_t at, int d) {
        if (at >= n || d < 0) return;            // past the end: the frame ends there
        if (d <= depth[at]) return;
        depth[at] = d;
        work.push_back(at);
    };
    reach(0, 0);
    while (!work.empty()) {
        const std::size_t i = work.back();
        work.pop_back();
        const int d = depth[i];
        deepest = std::max(deepest, d);
        const Instr word = code_[i];
        const Op op = static_cast<Op>(word & 0xFF);
        const std::size_t operand = word >> kOperandShift;
        const std::size_t next = i + 1;
        int after = d;
        switch (op) {
            case Op::PUSH_CONST: case Op::PUSH_VAR: case Op::PUSH_NIL: case Op::PUSH_TRUE:
            case Op::PUSH_FALSE: case Op::PUSH_LOCAL: case Op::PUSH_LOCAL_CLEAR: case Op::DUP:
            case Op::RESOLVE_CLASS:
                after = d + 1; break;
            case Op::POP: case Op::STORE_LOCAL: case Op::STORE_CAUGHT:
            case Op::ADD: case Op::SUB: case Op::MUL: case Op::LT: case Op::LE:
            case Op::GT: case Op::GE: case Op::EQ: case Op::CALL_APPLY:
            case Op::EXC_MATCH_TYPE:
                after = d - 1; break;
            case Op::NOP: case Op::STORE_GLOBAL: case Op::TRY_END: case Op::EXC_MATCH:
            case Op::GET_MEMBER: case Op::LINE:
                break;
            case Op::CALL: case Op::CALL_KW: case Op::CALL_MEMBER:
                after = d - static_cast<int>(operand); break;
            case Op::MAKE_FN:
                after = d + 1 - (operand < blocks_.size() ? blocks_[operand]->captureCount() : 0);
                break;
            case Op::MAKE_FN_MULTI: {
                int caps = 0;
                if (operand < arityGroups_.size())
                    for (std::size_t b : arityGroups_[operand].blockIndices)
                        if (b < blocks_.size()) caps += blocks_[b]->captureCount();
                after = d + 1 - caps;
                break;
            }
            case Op::JUMP:
                reach(next + operand, d);
                continue;
            case Op::JUMP_BACK:
                if (operand <= next) reach(next - operand, d);
                continue;
            case Op::JUMP_IF_FALSE: case Op::JUMP_IF_TRUE:
                reach(next + operand, d - 1);
                after = d - 1; break;
            case Op::TRY_BEGIN:
                reach(next + operand, d + 1);
                break;
            case Op::RETURN: case Op::THROW: case Op::RETHROW:
                continue;
            default:
                return 64;   // an opcode this analysis does not know: the old fixed capacity
        }
        // A push is counted at the instruction after it, which can be the end.
        deepest = std::max(deepest, after);
        reach(next, after);
    }
    return static_cast<unsigned int>(deepest);
}

} // namespace protoClojure
