#include "BytecodeModule.h"

#include <stdexcept>

namespace protoClojure {

const char* opName(Op op) {
    switch (op) {
        case Op::NOP:        return "NOP";
        case Op::PUSH_CONST: return "PUSH_CONST";
        case Op::PUSH_VAR:   return "PUSH_VAR";
        case Op::CALL:       return "CALL";
        case Op::POP:        return "POP";
        case Op::RETURN:     return "RETURN";
    }
    return "?";
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

void BytecodeModule::emit(Op op, std::uint8_t operand) {
    bytes_.push_back(static_cast<std::uint8_t>(op));
    bytes_.push_back(operand);
}

} // namespace protoClojure
