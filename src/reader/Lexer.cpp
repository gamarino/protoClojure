#include "Lexer.h"
#include "UnicodeLetters.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace protoClojure {

namespace {

// Decode the UTF-8 sequence starting at s[pos]. Returns its length in bytes
// (1 to 4) and stores the code point in *cp, or returns 0 when the bytes are
// not well-formed UTF-8: an invalid lead or stray continuation byte, a
// truncated sequence, an overlong encoding, a surrogate or a value beyond
// U+10FFFF.
std::size_t decodeUtf8(const std::string& s, std::size_t pos, char32_t* cp) {
    const unsigned b0 = static_cast<unsigned char>(s[pos]);
    std::size_t len = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        len = 2; value = b0 & 0x1F; minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3; value = b0 & 0x0F; minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4; value = b0 & 0x07; minimum = 0x10000;
    } else {
        return 0;
    }
    if (len > s.size() - pos) return 0;
    for (std::size_t i = 1; i < len; ++i) {
        const unsigned b = static_cast<unsigned char>(s[pos + i]);
        if ((b & 0xC0) != 0x80) return 0;
        value = (value << 6) | (b & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    *cp = value;
    return len;
}

} // namespace

const char* tokenKindName(TokenKind k) {
    switch (k) {
        case TokenKind::Integer:        return "Integer";
        case TokenKind::Float:          return "Float";
        case TokenKind::Ratio:          return "Ratio";
        case TokenKind::String:         return "String";
        case TokenKind::Char:           return "Char";
        case TokenKind::Symbol:         return "Symbol";
        case TokenKind::Keyword:        return "Keyword";
        case TokenKind::Nil:            return "Nil";
        case TokenKind::True:           return "True";
        case TokenKind::False:          return "False";
        case TokenKind::LParen:         return "LParen";
        case TokenKind::RParen:         return "RParen";
        case TokenKind::LBracket:       return "LBracket";
        case TokenKind::RBracket:       return "RBracket";
        case TokenKind::LBrace:         return "LBrace";
        case TokenKind::RBrace:         return "RBrace";
        case TokenKind::HashLBrace:     return "HashLBrace";
        case TokenKind::HashLParen:     return "HashLParen";
        case TokenKind::Quote:          return "Quote";
        case TokenKind::Backtick:       return "Backtick";
        case TokenKind::Tilde:          return "Tilde";
        case TokenKind::TildeAt:        return "TildeAt";
        case TokenKind::HashUnderscore: return "HashUnderscore";
        case TokenKind::HashApostrophe: return "HashApostrophe";
        case TokenKind::Caret:          return "Caret";
        case TokenKind::At:             return "At";
        case TokenKind::EndOfFile:      return "EOF";
        case TokenKind::Error:          return "Error";
    }
    return "?";
}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

// Columns count code points: a UTF-8 continuation byte does not advance the
// column.
void Lexer::advance() {
    if (eof()) return;
    const unsigned char c = static_cast<unsigned char>(source_[pos_]);
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else if ((c & 0xC0) != 0x80) {
        ++column_;
    }
    ++pos_;
}

// Clojure treats commas as whitespace ([1, 2, 3] is identical to [1 2 3]).
// Line comments start with `;` and run to end of line. Block comments do not
// exist (Clojure uses `#_` form-discarding for compile-time skipping; that is
// a reader-level concern, not a lexer one).
void Lexer::skipWhitespaceAndComments() {
    while (!eof()) {
        char c = current();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            advance();
            continue;
        }
        if (c == ';') {
            while (!eof() && current() != '\n') advance();
            continue;
        }
        break;
    }
}

// "Operand" matches protoST's D1 definition — a `-` is the start of a
// negative numeric literal only when not following a token that closes a value
// position. After a literal or a `)`, `-` is a binary subtract; in primary
// position it is a sign.
bool Lexer::prevEndsOperand() const {
    switch (prevReturnedKind_) {
        case TokenKind::Integer:
        case TokenKind::Float:
        case TokenKind::Ratio:
        case TokenKind::String:
        case TokenKind::Char:
        case TokenKind::Symbol:
        case TokenKind::Keyword:
        case TokenKind::Nil:
        case TokenKind::True:
        case TokenKind::False:
        case TokenKind::RParen:
        case TokenKind::RBracket:
        case TokenKind::RBrace:
            return true;
        default:
            return false;
    }
}

Token Lexer::makeError(const std::string& msg, int errLine, int errCol) {
    Token t;
    t.kind = TokenKind::Error;
    t.text = msg;
    t.line = errLine;
    t.column = errCol;
    return t;
}

Token Lexer::next() {
    Token t = nextImpl_();
    prevReturnedKind_ = t.kind;
    return t;
}

const Token& Lexer::peek() {
    if (!hasPeek_) {
        peekTok_ = next();
        hasPeek_ = true;
    }
    return peekTok_;
}

Token Lexer::nextImpl_() {
    if (hasPeek_) {
        hasPeek_ = false;
        return peekTok_;
    }

    std::size_t beforeWs = pos_;
    skipWhitespaceAndComments();

    if (eof()) {
        Token t; t.kind = TokenKind::EndOfFile;
        t.line = line_; t.column = column_;
        return t;
    }

    bool spaceBefore = (pos_ != beforeWs);
    char c = current();
    int startLine = line_, startCol = column_;

    // Single-char punctuation that does not depend on lookahead.
    auto single = [&](TokenKind k) -> Token {
        Token t; t.kind = k; t.text = std::string(1, c);
        t.line = startLine; t.column = startCol;
        advance();
        return t;
    };

    switch (c) {
        case '(': return single(TokenKind::LParen);
        case ')': return single(TokenKind::RParen);
        case '[': return single(TokenKind::LBracket);
        case ']': return single(TokenKind::RBracket);
        case '{': return single(TokenKind::LBrace);
        case '}': return single(TokenKind::RBrace);
        case '@': return single(TokenKind::At);
        // Hash-prefixes and the reader-macro prefixes still go through
        // the reserved-for-later path so we surface a clean error if the
        // user types them.
        default:  break;
    }

    if (c == '"') return lexString();

    if (std::isdigit(static_cast<unsigned char>(c))) {
        return lexNumber(/*negative=*/false);
    }

    // A `-` immediately followed by a digit, in primary position, is the sign
    // of a negative literal. Otherwise it is the start of a symbol (which
    // includes the operator selectors `+`, `-`, `*`, etc. in Clojure).
    if (c == '-' && std::isdigit(static_cast<unsigned char>(lookahead()))
        && (!prevEndsOperand() || spaceBefore)) {
        advance();  // consume the `-`
        Token t = lexNumber(/*negative=*/true);
        return t;
    }

    return lexSymbolOrPunct();
}

Token Lexer::lexNumber(bool negative) {
    int startLine = line_, startCol = column_;
    std::string digits;
    if (negative) digits += '-';
    while (!eof() && std::isdigit(static_cast<unsigned char>(current()))) {
        digits += current();
        advance();
    }

    // Session 9 — optional fractional / exponent part for floats. A `.` not
    // followed by a digit is rejected (matches Clojure-JVM: `1.` is allowed
    // there but produces 1.0; we are stricter for now and require at least
    // one fractional digit).
    bool isFloat = false;
    if (!eof() && current() == '.') {
        char la = lookahead();
        if (std::isdigit(static_cast<unsigned char>(la))) {
            isFloat = true;
            digits += '.';
            advance();
            while (!eof() && std::isdigit(static_cast<unsigned char>(current()))) {
                digits += current();
                advance();
            }
        }
    }
    if (!eof() && (current() == 'e' || current() == 'E')) {
        isFloat = true;
        digits += current();
        advance();
        if (!eof() && (current() == '+' || current() == '-')) {
            digits += current();
            advance();
        }
        if (eof() || !std::isdigit(static_cast<unsigned char>(current()))) {
            return makeError("malformed number literal: " + digits + "(missing exponent digits)",
                             startLine, startCol);
        }
        while (!eof() && std::isdigit(static_cast<unsigned char>(current()))) {
            digits += current();
            advance();
        }
    }

    // Clojure's big-integer suffix, `42N`. Integers already promote to a
    // LargeInteger when they need to (D14), so the suffix only marks the
    // literal and does not change its value; a float cannot take it.
    bool bigSuffix = false;
    if (!isFloat && !eof() && current() == 'N') {
        bigSuffix = true;
        advance();
    }

    // Numbers followed by a letter, `_` or `.` (e.g. `42x`, `42ñ`, `1.5N`)
    // are an error — JVM Clojure also rejects them. The digit loops above
    // leave no ASCII digit at the current position.
    if (!eof()) {
        // Byte length of the character at `pos` when it continues a
        // malformed number: an ASCII letter or digit, `_`, `.`, or a
        // non-ASCII symbol letter; 0 otherwise.
        auto trailLength = [&](std::size_t pos) -> std::size_t {
            const unsigned char ch = static_cast<unsigned char>(source_[pos]);
            if (ch >= 0x80) return symbolCharLength(pos);
            return (std::isalnum(ch) || ch == '_' || ch == '.') ? 1 : 0;
        };
        if (trailLength(pos_) > 0) {
            std::string bad = digits;
            if (bigSuffix) bad += 'N';
            while (!eof()) {
                const std::size_t len = trailLength(pos_);
                if (len == 0) break;
                bad.append(source_, pos_, len);
                for (std::size_t i = 0; i < len; ++i) advance();
            }
            return makeError("malformed number literal: " + bad,
                             startLine, startCol);
        }
    }
    Token t;
    t.text = digits;
    t.line = startLine;
    t.column = startCol;
    if (isFloat) {
        t.kind = TokenKind::Float;
        t.doubleValue = std::strtod(digits.c_str(), nullptr);
    } else {
        t.kind = TokenKind::Integer;
        // strtoll clamps an out-of-range literal to LLONG_MIN / LLONG_MAX;
        // such a literal keeps only its digits, which the Reader turns into
        // an exact LargeInteger.
        errno = 0;
        const long long value = std::strtoll(digits.c_str(), nullptr, 10);
        if (errno == ERANGE) t.fitsLong = false;
        else                 t.intValue = value;
    }
    return t;
}

Token Lexer::lexString() {
    int startLine = line_, startCol = column_;
    advance(); // consume opening "
    std::string out;
    while (!eof() && current() != '"') {
        if (current() == '\\') {
            advance();
            if (eof()) {
                return makeError("unterminated string escape", startLine, startCol);
            }
            char esc = current();
            switch (esc) {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"':  out += '"';  break;
                case '0':  out += '\0'; break;
                default:
                    // Unicode `\uXXXX` and octal `\oNNN` reserved.
                    return makeError(
                        std::string("unsupported string escape: \\") + esc,
                        line_, column_);
            }
            advance();
        } else {
            out += current();
            advance();
        }
    }
    if (eof()) {
        return makeError("unterminated string literal", startLine, startCol);
    }
    advance(); // consume closing "
    Token t;
    t.kind = TokenKind::String;
    t.text = std::move(out);
    t.line = startLine;
    t.column = startCol;
    return t;
}

// Anything not handled above and not single-punct is read as a symbol.
// Clojure symbols accept letters, digits, and the punctuation set
// `* + ! - _ ' ? < > = . / :`. The first char cannot be a bare digit
// (handled above) but may be any of the punctuation chars. Beyond ASCII, a
// symbol accepts UTF-8 encoded Unicode letters, combining marks and decimal
// digits (UnicodeLetters.h); any other non-ASCII character, and malformed
// UTF-8, ends the symbol and is a read error on its own.
//
// Reserved-for-later characters surface a clear error so the user knows
// the token category exists but is not yet wired.
static bool isAsciiSymbolChar(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    switch (c) {
        case '*': case '+': case '!': case '-': case '_': case '\'':
        case '?': case '<': case '>': case '=': case '.': case '/':
        case ':': case '&': case '$': case '%':
            return true;
        default:
            return false;
    }
}

std::size_t Lexer::symbolCharLength(std::size_t pos) const {
    const unsigned char c = static_cast<unsigned char>(source_[pos]);
    if (c < 0x80) return isAsciiSymbolChar(static_cast<char>(c)) ? 1 : 0;
    char32_t cp = 0;
    const std::size_t len = decodeUtf8(source_, pos, &cp);
    return (len > 0 && isSymbolLetter(cp)) ? len : 0;
}

Token Lexer::lexSymbolOrPunct() {
    int startLine = line_, startCol = column_;
    char c = current();

    // Reader-macro prefixes that we recognise but do not yet handle in v0.0.x.
    // Returning a clear Error gives the user a useful message and lets the
    // unit test for "reserved tokens" prove they are detected, not silently
    // dropped.
    auto reserved = [&](const std::string& name) -> Token {
        return makeError("reserved-for-later token: " + name, startLine, startCol);
    };

    if (c == '\'' || c == '`' || c == '~' || c == '^') {
        std::string s(1, c);
        advance();
        return reserved(s);
    }
    if (c == '#') {
        char la = lookahead();
        if (la == '{' || la == '(' || la == '_' || la == '\'' || la == '"') {
            std::string s = std::string("#") + la;
            advance(); advance();
            return reserved(s);
        }
        std::string s = "#";
        advance();
        return reserved(s);
    }

    // Symbol body.
    if (symbolCharLength(pos_) == 0) {
        char32_t cp = 0;
        const std::size_t len = decodeUtf8(source_, pos_, &cp);
        if (len == 0) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "0x%02X",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            advance();
            return makeError(std::string("invalid UTF-8 byte ") + hex,
                             startLine, startCol);
        }
        // The whole character, plus its code point when it is not ASCII
        // (it may be invisible, such as a no-break space).
        std::string bad = source_.substr(pos_, len);
        if (len > 1) {
            char codePoint[16];
            std::snprintf(codePoint, sizeof codePoint, " (U+%04X)",
                          static_cast<unsigned>(cp));
            bad += codePoint;
        }
        for (std::size_t i = 0; i < len; ++i) advance();
        return makeError("unexpected character: " + bad, startLine, startCol);
    }
    std::string s;
    while (!eof()) {
        const std::size_t len = symbolCharLength(pos_);
        if (len == 0) break;
        s.append(source_, pos_, len);
        for (std::size_t i = 0; i < len; ++i) advance();
    }
    Token t;
    t.kind = TokenKind::Symbol;
    t.text = std::move(s);
    t.line = startLine;
    t.column = startCol;
    return t;
}

} // namespace protoClojure
