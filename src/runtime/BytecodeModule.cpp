#include "BytecodeModule.h"

#include <stdexcept>

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
    }
    return "?";
}

std::size_t BytecodeModule::addBlock(std::unique_ptr<BytecodeModule> sub) {
    blocks_.push_back(std::move(sub));
    return blocks_.size() - 1;
}

std::size_t BytecodeModule::addLong(long long v) {
    consts_.push_back(Const{ConstKind::Long, v, {}});
    return consts_.size() - 1;
}

std::size_t BytecodeModule::addString(const std::string& s) {
    consts_.push_back(Const{ConstKind::String, 0, s});
    return consts_.size() - 1;
}

std::size_t BytecodeModule::addSymbol(const std::string& s) {
    // De-duplicate symbols: identical names produce the same const index.
    // Matters because the compiler often references the same global (e.g.
    // `println`) multiple times across a file; we should not multiply
    // entries.
    for (std::size_t i = 0; i < consts_.size(); ++i) {
        if (consts_[i].kind == ConstKind::Symbol && consts_[i].sval == s) {
            return i;
        }
    }
    consts_.push_back(Const{ConstKind::Symbol, 0, s});
    return consts_.size() - 1;
}

std::size_t BytecodeModule::emit(Op op, std::uint8_t operand) {
    std::size_t at = bytes_.size();
    bytes_.push_back(static_cast<std::uint8_t>(op));
    bytes_.push_back(operand);
    return at;
}

void BytecodeModule::patchOperand(std::size_t opcodeOffset,
                                  std::uint8_t newOperand) {
    if (opcodeOffset + 1 >= bytes_.size()) {
        throw std::runtime_error("patchOperand: out of range");
    }
    bytes_[opcodeOffset + 1] = newOperand;
}

} // namespace protoClojure
