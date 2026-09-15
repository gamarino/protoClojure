#include "BytecodeModule.h"

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

std::size_t BytecodeModule::addSymbol(const std::string& s) {
    return findOrAdd(symbolIndex_, s, consts_, Const{ConstKind::Symbol, 0, 0.0, s});
}

std::size_t BytecodeModule::addNamed(const std::string& spelling,
                                     const proto::ProtoObject* value) {
    return findOrAdd(namedIndex_, spelling, consts_,
                     Const{ConstKind::Named, 0, 0.0, spelling, value});
}

std::size_t BytecodeModule::emit(Op op, std::size_t operand) {
    const std::uint32_t checked = checkedOperand(op, operand);
    code_.push_back((checked << kOperandShift) | static_cast<Instr>(op));
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

} // namespace protoClojure
