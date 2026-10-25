#include "Disassembler.h"

#include "BytecodeModule.h"
#include "Exceptions.h"
#include "Macros.h"
#include "Opcodes.h"
#include "Primitives.h"

#include "protoCore.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace protoClojure {

namespace {

std::atomic<std::FILE*> gOutput{nullptr};
std::atomic<int> gFormNumber{0};

// A string constant as a Clojure literal: quoted, with `"`, `\` and the
// control characters escaped.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

const char* kindName(BytecodeModule::ConstKind k) {
    using K = BytecodeModule::ConstKind;
    switch (k) {
        case K::Long:       return "long";
        case K::Double:     return "double";
        case K::String:     return "string";
        case K::Symbol:     return "name";
        case K::Named:      return "named";
        case K::BigInteger: return "bigint";
        case K::Datum:      return "datum";
    }
    return "?";
}

// A constant as the listing shows it.
std::string constText(proto::ProtoContext* ctx, const BytecodeModule::Const& c) {
    using K = BytecodeModule::ConstKind;
    switch (c.kind) {
        case K::Long:       return std::to_string(c.ival);
        case K::Double:     return formatDouble(c.dval);
        case K::String:     return quoted(c.sval);
        case K::Symbol:     return c.sval;
        case K::Named:      return c.sval;
        case K::BigInteger: return c.sval + "N";
        case K::Datum: {
            std::string out;
            printValueTo(ctx, out, c.named, /*readable=*/true);
            return out;
        }
    }
    return "?";
}

// What a local slot of `mod` holds: a parameter, the rest parameter, a
// `:keys` or `:as` parameter, a closure capture, or a let / loop / catch
// local.
std::string slotRole(const BytecodeModule& mod, std::uint32_t slot) {
    const int s = static_cast<int>(slot);
    if (s < mod.arity()) return "param " + std::to_string(s);
    if (mod.isVariadic() && s == mod.arity()) return "rest param";
    for (const auto& k : mod.kwKeys())
        if (k.localSlot == s) return ":keys " + k.name;
    if (mod.asSlot() == s) return ":as param";
    for (const auto& c : mod.captureSpecs())
        if (c.localSlot == s) return "capture (parent slot " + std::to_string(c.parentSlot) + ")";
    return "local";
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

void listModule(proto::ProtoContext* ctx, const BytecodeModule& mod, const std::string& name,
                std::string& out) {
    // The header: the frame layout the VM builds for this body.
    out += "module " + name + ": arity " + std::to_string(mod.arity());
    if (mod.isVariadic()) out += " + rest";
    if (mod.isKwBased()) out += " + keyword args";
    out += ", locals " + std::to_string(mod.localCount()) +
           ", max stack " + std::to_string(mod.maxStack());
    if (mod.hasHandlers()) out += ", has handlers";
    out += "\n";
    if (mod.captureCount() > 0) {
        out += "  captures:";
        for (const auto& c : mod.captureSpecs())
            out += " parent slot " + std::to_string(c.parentSlot) + " -> slot " +
                   std::to_string(c.localSlot) + ";";
        out.pop_back();
        out += "\n";
    }
    if (mod.isKwBased()) {
        out += "  keyword args:";
        for (const auto& k : mod.kwKeys())
            out += " :" + k.name + " -> slot " + std::to_string(k.localSlot) + ";";
        if (mod.asSlot() >= 0) out += " :as -> slot " + std::to_string(mod.asSlot()) + ";";
        out.pop_back();
        out += "\n";
    }
    if (mod.constCount() > 0) {
        out += "  constants:\n";
        for (std::size_t i = 0; i < mod.constCount(); ++i) {
            const auto& c = mod.constAt(i);
            out += "    " + pad("#" + std::to_string(i), 5) + pad(kindName(c.kind), 8) +
                   constText(ctx, c) + "\n";
        }
    }
    for (std::size_t g = 0; g < mod.arityGroupCount(); ++g) {
        out += "  arity group " + std::to_string(g) + ": blocks";
        for (std::size_t b : mod.arityGroup(g).blockIndices) out += " " + std::to_string(b);
        out += "\n";
    }

    out += "  code:\n";
    const auto& code = mod.code();
    const int width = static_cast<int>(std::to_string(code.size()).size());
    bool memberWord = false;   // the next word is CALL_MEMBER's data word
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
        const Instr word = code[pc];
        const Op op = static_cast<Op>(word & 0xFF);
        const std::uint32_t operand = word >> kOperandShift;
        char pcText[24];
        std::snprintf(pcText, sizeof pcText, "%*zu", width, pc);
        std::string line = std::string("    ") + pcText + "  " + pad(opName(op), 17);
        std::string operandText = std::to_string(operand);
        std::string note;
        auto constNote = [&]() {
            note = operand < mod.constCount() ? constText(ctx, mod.constAt(operand))
                                              : "constant out of range";
        };
        if (memberWord) {
            memberWord = false;
            constNote();
            note = "data word of CALL_MEMBER: member " + note;
            line += operandText;
        } else {
            switch (op) {
                case Op::NOP: case Op::POP: case Op::RETURN: case Op::PUSH_NIL:
                case Op::PUSH_TRUE: case Op::PUSH_FALSE: case Op::DUP:
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::LT: case Op::LE:
                case Op::GT: case Op::GE: case Op::EQ: case Op::TRY_END: case Op::THROW:
                case Op::CALL_APPLY:
                    // No operand (reserved, always 0); show one only if set.
                    if (operand != 0) line += operandText;
                    break;
                case Op::PUSH_CONST: case Op::PUSH_VAR: case Op::STORE_GLOBAL:
                case Op::GET_MEMBER: case Op::RESOLVE_CLASS: case Op::EXC_MATCH_TYPE:
                    line += operandText;
                    constNote();
                    break;
                case Op::PUSH_LOCAL: case Op::PUSH_LOCAL_CLEAR: case Op::STORE_LOCAL:
                case Op::STORE_CAUGHT: case Op::RETHROW:
                    line += operandText;
                    note = slotRole(mod, operand);
                    break;
                case Op::JUMP: case Op::JUMP_IF_FALSE: case Op::JUMP_IF_TRUE:
                case Op::TRY_BEGIN:
                    line += operandText;
                    note = "-> " + std::to_string(pc + 1 + operand);
                    break;
                case Op::JUMP_BACK:
                    line += operandText;
                    note = "-> " + std::to_string(pc + 1 - operand);
                    break;
                case Op::CALL: case Op::CALL_KW:
                    line += operandText;
                    note = operand == 1 ? "1 arg" : operandText + " args";
                    break;
                case Op::CALL_MEMBER:
                    line += operandText;
                    note = operand == 1 ? "1 arg" : operandText + " args";
                    memberWord = true;
                    break;
                case Op::MAKE_FN:
                    line += operandText;
                    note = "fn " + name + "." + operandText;
                    break;
                case Op::MAKE_FN_MULTI: {
                    line += operandText;
                    note = "multi-arity fn:";
                    if (operand < mod.arityGroupCount())
                        for (std::size_t b : mod.arityGroup(operand).blockIndices)
                            note += " " + name + "." + std::to_string(b);
                    break;
                }
                case Op::EXC_MATCH: {
                    line += operandText;
                    const char* cls = exceptionClassName(static_cast<int>(operand));
                    note = cls ? cls : "unknown class";
                    break;
                }
                case Op::LINE:
                    line += operandText;
                    note = operand == 0 ? "forget the current line" : "source line";
                    break;
                default:
                    line += operandText;
                    note = "unknown opcode " + std::to_string(static_cast<unsigned>(op));
                    break;
            }
        }
        if (!note.empty()) line = pad(line, 34) + "; " + note;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    for (std::size_t b = 0; b < mod.blockCount(); ++b) {
        out += "\n";
        listModule(ctx, mod.block(b), name + "." + std::to_string(b), out);
    }
}

} // namespace

std::string disassemble(proto::ProtoContext* ctx, const BytecodeModule& mod,
                        const std::string& name) {
    std::string out;
    listModule(ctx, mod, name, out);
    return out;
}

void setDisassemblyOutput(std::FILE* out) { gOutput.store(out); }

std::FILE* disassemblyOutput() { return gOutput.load(); }

void disassembleTopLevel(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                         const BytecodeModule& mod) {
    std::FILE* out = gOutput.load();
    if (!out || coreNamespaceActive()) return;
    const int n = ++gFormNumber;
    std::string text = ";; form " + std::to_string(n) + ": ";
    printValueTo(ctx, text, form, /*readable=*/true);
    text += "\n";
    text += disassemble(ctx, mod, "form" + std::to_string(n));
    text += "\n";
    std::fwrite(text.data(), 1, text.size(), out);
    std::fflush(out);
}

} // namespace protoClojure
