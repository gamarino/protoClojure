#include "Reader.h"

#include "runtime/ListBuilder.h"
#include "runtime/MapOps.h"
#include "runtime/Named.h"
#include "runtime/SetOps.h"
#include "runtime/Regex.h"
#include "runtime/StackGuard.h"
#include "runtime/Utf8String.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"

#include <vector>

namespace protoClojure {

Reader::Reader(proto::ProtoContext* ctx, std::string source,
               const NamedLayout& named, SourceInfo* info)
    : ctx_(ctx), lexer_(std::move(source)), named_(named), info_(info) {}

namespace {

// How a map-literal key is spelled in the "Duplicate key" read error: the
// source text of a keyword, a symbol, a string or an integer, and a generic
// description of anything else (the reader has no printer of its own).
std::string describeKey(proto::ProtoContext* ctx, const NamedLayout& named,
                        const proto::ProtoObject* key) {
    if (key == PROTO_NONE) return "nil";
    if (key == PROTO_TRUE) return "true";
    if (key == PROTO_FALSE) return "false";
    if (isNamed(ctx, named, key)) return namedSpelling(ctx, named, key)->toStdString(ctx);
    if (proto::ProtoObject::isStringTagFast(key))
        return "\"" + reinterpret_cast<const proto::ProtoString*>(key)->toStdString(ctx) + "\"";
    if (key->isInteger(ctx)) return key->asIntegerString(ctx, 10)->toStdString(ctx);
    return "a repeated form";
}

} // namespace

// Forms nested deeper than the thread's stack allows raise StackOverflowError
// in readFromToken; readOne and readAll report it as a read error at the
// position where reading stopped, so every driver handles it like any other
// read error.
const proto::ProtoObject* Reader::readOne() {
    return readOne(ctx_);
}

const proto::ProtoObject* Reader::readOne(proto::ProtoContext* ctx) {
    try {
        Token tok = lexer_.next();
        while (tok.kind == TokenKind::HashUnderscore) {
            discardNextForm(ctx, tok);
            tok = lexer_.next();
        }
        if (tok.kind == TokenKind::EndOfFile) return nullptr;
        formLine_ = tok.line;
        formColumn_ = tok.column;
        return readFromToken(ctx, tok);
    } catch (const StackOverflowError& e) {
        throw ReaderError(e.what(), lexer_.line(), lexer_.column());
    }
}

const proto::ProtoList* Reader::readAll() {
    try {
        return readAllForms();
    } catch (const StackOverflowError& e) {
        throw ReaderError(e.what(), lexer_.line(), lexer_.column());
    }
}

const proto::ProtoList* Reader::readAllForms() {
    // The accumulator IS a ProtoList the protoCore GC traces — no std
    // container in sight — and it is grown in chunks, so the intermediate
    // versions a long file produces are reclaimed while the file is still
    // being read instead of being pinned until the last form is parsed.
    // Forms are read through the builder's context: a thread's contexts form
    // a LIFO stack and the builder must stay its innermost frame while it is
    // open (see ListBuilder.h).
    ListBuilder forms(ctx_);

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) break;
        if (tok.kind == TokenKind::HashUnderscore) {
            discardNextForm(forms.context(), tok);
            continue;
        }
        forms.push(readFromToken(forms.context(), tok));
    }
    return forms.finishList();
}

void Reader::discardNextForm(proto::ProtoContext* parent, const Token& tok) {
    // The discarded form is built like any other (it may hold a nested
    // `#_`), in a scope of its own that drops it.
    proto::ProtoContext scope(parent->space, parent);
    (void)readNextForm(&scope, tok);
}

const proto::ProtoObject* Reader::fnLiteralArg(proto::ProtoContext* ctx, const std::string& text) {
    auto gen = [&](const std::string& stem) {
        return internNamed(ctx, named_, (stem + "__" + std::to_string(nextGensymId()) + "#").c_str());
    };
    if (text == "%&") {
        if (!fnLiteral_->rest) fnLiteral_->rest = gen("rest");
        return fnLiteral_->rest;
    }
    std::size_t n = 1;
    if (text.size() > 1) {
        n = 0;
        for (std::size_t i = 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return nullptr;
            n = n * 10 + static_cast<std::size_t>(text[i] - '0');
            if (n > 20) throw ReaderError("Arg literal must be %, %& or %integer (at most %20)", 0, 0);
        }
        if (n == 0) throw ReaderError("Arg literal must be %, %& or %integer", 0, 0);
    }
    if (fnLiteral_->params.size() < n) fnLiteral_->params.resize(n, nullptr);
    if (!fnLiteral_->params[n - 1]) fnLiteral_->params[n - 1] = gen("p" + std::to_string(n));
    return fnLiteral_->params[n - 1];
}

const proto::ProtoObject*
Reader::readFromToken(proto::ProtoContext* parent, const Token& tok) {
    // Every nested form recurses through here (readList, `@`).
    checkNativeStack(StackUse::Source);
    if (edn_) {
        // EDN is data: the reader macros that only make sense in code are
        // errors. `#(` and `#"` get the message of Clojure's EdnReader.
        switch (tok.kind) {
            case TokenKind::HashLParen:
                throw ReaderError("No dispatch macro for: (", tok.line, tok.column);
            case TokenKind::Regex:
                throw ReaderError("No dispatch macro for: \"", tok.line, tok.column);
            case TokenKind::Quote: case TokenKind::Backtick: case TokenKind::Tilde:
            case TokenKind::TildeAt: case TokenKind::At:
                throw ReaderError("EDN does not support the reader macro " + tok.text,
                                  tok.line, tok.column);
            default: break;
        }
    }
    switch (tok.kind) {
        case TokenKind::HashUnderscore: {
            // `#_x y` in a position that takes one form (`'#_x y`): x is
            // dropped and the form is y.
            discardNextForm(parent, tok);
            return readNextForm(parent, tok);
        }

        case TokenKind::Integer:
            // Atomic — no intermediate state to protect. The caller takes
            // ownership of rooting the return value. A literal beyond the
            // long long range is built from its digits as an exact
            // LargeInteger (protoCore parses under its own critical section).
            if (!tok.fitsLong) return parent->fromString(tok.text.c_str(), 10);
            return parent->fromLong(tok.intValue);

        case TokenKind::Float:
            return parent->fromDouble(tok.doubleValue);

        case TokenKind::String:
            // A string literal is the protoCore string a program builds at
            // run time. Symbols are named values (runtime/Named.h), never
            // strings, so nothing is lost when protoCore stores a short
            // string inline in the pointer.
            return makeUtf8String(parent, tok.text);  // NULs included

        case TokenKind::Symbol: {
            // `nil`, `true` and `false` are the values themselves, as in
            // Clojure's reader. Every other symbol and every keyword (the
            // lexer spells a keyword with its leading `:`) is the interned
            // named value of its spelling: one pointer per spelling for the
            // life of the process, the same value `(quote foo)` and `:foo`
            // evaluate to.
            if (tok.text == "nil")   return PROTO_NONE;
            if (tok.text == "true")  return PROTO_TRUE;
            if (tok.text == "false") return PROTO_FALSE;
            if (fnLiteral_ && !tok.text.empty() && tok.text[0] == '%') {
                if (const proto::ProtoObject* p = fnLiteralArg(parent, tok.text)) return p;
            }
            return internNamed(parent, named_, tok.text.c_str());
        }

        case TokenKind::HashLParen: {
            // `#(body)`: `(fn* [p1 ... pN & rest] (body))`, where `%` and
            // `%1` .. `%N` name the positional parameters, N the largest
            // index used, and `%&` the rest, as Clojure's reader writes it.
            // The parameters are generated symbols, `p1__N#`, interned like
            // every symbol.
            if (fnLiteral_) throw ReaderError("Nested #()s are not allowed", tok.line, tok.column);
            FnLiteral lit;
            fnLiteral_ = &lit;
            struct Reset { FnLiteral*& f; ~Reset() { f = nullptr; } } reset{fnLiteral_};
            proto::ProtoContext scope(parent->space, parent);
            scope.resizeAutomaticLocals(2);
            scope.setAutomaticLocal(0, readList(&scope, TokenKind::RParen, tok.line, tok.column));
            const proto::ProtoList* params = scope.newList();
            for (std::size_t i = 0; i < lit.params.size(); ++i) {
                if (!lit.params[i])
                    lit.params[i] = internNamed(&scope, named_,
                        ("p" + std::to_string(i + 1) + "__" + std::to_string(nextGensymId()) + "#").c_str());
                params = params->appendLast(&scope, lit.params[i]);
            }
            if (lit.rest)
                params = params->appendLast(&scope, internNamed(&scope, named_, "&"))
                               ->appendLast(&scope, lit.rest);
            scope.setAutomaticLocal(1, newVector(&scope, params));
            const proto::ProtoObject* form = scope.newList()
                ->appendLast(&scope, internNamed(&scope, named_, "fn*"))
                ->appendLast(&scope, scope.getAutomaticLocal(1))
                ->appendLast(&scope, scope.getAutomaticLocal(0))->asObject(&scope);
            scope.returnValue = form;
            return form;
        }

        case TokenKind::LParen:
            return readList(parent, TokenKind::RParen,
                            tok.line, tok.column);

        // The reader macros that stand for a two-element list: `@x` is
        // `(clojure.core/deref x)`, `'x` is `(quote x)`, `~x` is
        // `(clojure.core/unquote x)` and `~@x` is
        // `(clojure.core/unquote-splicing x)`, as Clojure's reader spells
        // them. `quote` is a special form and stays unqualified.
        case TokenKind::At:
            return readWrapped(parent, tok, "clojure.core/deref");
        case TokenKind::Quote:
            return readWrapped(parent, tok, "quote");
        case TokenKind::Tilde:
            return readWrapped(parent, tok, "clojure.core/unquote");
        case TokenKind::TildeAt:
            return readWrapped(parent, tok, "clojure.core/unquote-splicing");

        case TokenKind::Backtick: {
            // `` `form ``: syntax-quote, expanded here, at read time, into
            // the forms that build `form`, as Clojure's reader does
            // (syntaxQuote below).
            proto::ProtoContext scope(parent->space, parent);
            scope.resizeAutomaticLocals(1);
            scope.setAutomaticLocal(0, readNextForm(&scope, tok));
            // Each syntax-quote has its own auto-gensym table; one nested in
            // `form` was expanded, with its own, while `form` was read.
            gensyms_.emplace_back();
            struct Pop {
                std::vector<std::unordered_map<std::string, const proto::ProtoObject*>>& g;
                ~Pop() { g.pop_back(); }
            } pop{gensyms_};
            return syntaxQuote(&scope, scope.getAutomaticLocal(0), tok);
        }

        case TokenKind::LBracket: {
            // `[..]` is a vector, the value `(vector ..)` builds at run time
            // (runtime/VectorOps.h): the items are read into a ProtoList and
            // boxed, an O(1) step.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(1);
            wrapScope.setAutomaticLocal(0,
                readList(&wrapScope, TokenKind::RBracket, tok.line, tok.column));
            return newVector(&wrapScope,
                wrapScope.getAutomaticLocal(0)->asList(&wrapScope));
        }

        case TokenKind::LBrace: {
            // `{k1 v1 k2 v2 ...}` is a map, the value `(hash-map ..)` builds
            // at run time (runtime/MapOps.h). The forms are read in source
            // order into a ProtoList, then associated. As in Clojure, an odd
            // number of forms and a repeated key are read errors.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(1);
            wrapScope.setAutomaticLocal(0,
                readList(&wrapScope, TokenKind::RBrace, tok.line, tok.column));
            const proto::ProtoList* lst =
                wrapScope.getAutomaticLocal(0)->asList(&wrapScope);
            const proto::proto_ulong n = lst->getSize(&wrapScope);
            if (n % 2 != 0) {
                throw ReaderError("Map literal must contain an even number of forms",
                                  tok.line, tok.column);
            }
            // The forms stay reachable through the rooted list while the map
            // is built; protoCore does not move objects.
            std::vector<const proto::ProtoObject*> kv;
            kv.reserve(n);
            for (proto::proto_ulong i = 0; i < n; ++i)
                kv.push_back(lst->getAt(&wrapScope, static_cast<int>(i)));
            const proto::ProtoObject* m =
                mapAssocPairs(&wrapScope, nullptr, kv.data(), n);
            if (mapCount(&wrapScope, m) != n / 2) {
                for (proto::proto_ulong i = 2; i < n; i += 2) {
                    for (proto::proto_ulong j = 0; j < i; j += 2) {
                        if (keyEquals(&wrapScope, kv[j], kv[i])) {
                            throw ReaderError(
                                "Duplicate key: " + describeKey(&wrapScope, named_, kv[i]),
                                tok.line, tok.column);
                        }
                    }
                }
            }
            if (info_ && n > 0) {
                std::vector<const proto::ProtoObject*>& keys = info_->mapKeyOrder[m];
                keys.clear();
                for (proto::proto_ulong i = 0; i < n; i += 2) keys.push_back(kv[i]);
            }
            return m;
        }

        case TokenKind::Regex:
            // `#"..."` is a pattern value, compiled when it is read, as in
            // Clojure (runtime/Regex.h); a pattern the engine rejects is a
            // read error.
            try {
                return makePattern(parent, tok.text);
            } catch (const PatternSyntaxError& e) {
                throw ReaderError(std::string("PatternSyntaxException: ") + e.what(),
                                  tok.line, tok.column);
            }

        case TokenKind::HashLBrace: {
            // `#{a b}` is a set, the value `(hash-set ..)` builds at run time
            // (runtime/SetOps.h). As in Clojure, a repeated element is a read
            // error.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(1);
            wrapScope.setAutomaticLocal(0,
                readList(&wrapScope, TokenKind::RBrace, tok.line, tok.column,
                         "unterminated set — missing `}`"));
            const proto::ProtoList* lst =
                wrapScope.getAutomaticLocal(0)->asList(&wrapScope);
            const proto::proto_ulong n = lst->getSize(&wrapScope);
            std::vector<const proto::ProtoObject*> items;
            items.reserve(n);
            for (proto::proto_ulong i = 0; i < n; ++i)
                items.push_back(lst->getAt(&wrapScope, static_cast<int>(i)));
            const proto::ProtoObject* s = setConj(&wrapScope, nullptr, items.data(), n);
            if (setCount(&wrapScope, s) != n) {
                for (proto::proto_ulong i = 1; i < n; ++i) {
                    for (proto::proto_ulong j = 0; j < i; ++j) {
                        if (keyEquals(&wrapScope, items[j], items[i])) {
                            throw ReaderError(
                                "Duplicate key: " + describeKey(&wrapScope, named_, items[i]),
                                tok.line, tok.column);
                        }
                    }
                }
            }
            return s;
        }

        case TokenKind::Error:
            throw ReaderError(tok.text, tok.line, tok.column);

        case TokenKind::RParen:
            throw ReaderError("unexpected )", tok.line, tok.column);

        case TokenKind::EndOfFile:
            throw ReaderError("unexpected end of file", tok.line, tok.column);

        default:
            throw ReaderError(
                std::string("unsupported token: ") +
                tokenKindName(tok.kind),
                tok.line, tok.column);
    }
}

const proto::ProtoObject* Reader::readNextForm(proto::ProtoContext* parent,
                                               const Token& macroTok) {
    Token next = lexer_.next();
    if (next.kind == TokenKind::EndOfFile) {
        throw ReaderError("EOF while reading: " + macroTok.text +
                          " must be followed by a form",
                          macroTok.line, macroTok.column);
    }
    return readFromToken(parent, next);
}

const proto::ProtoObject* Reader::readWrapped(proto::ProtoContext* parent,
                                              const Token& macroTok,
                                              const char* head) {
    // The form is rooted in a slot of a child scope while the list is
    // built; the head symbol is an interned (never collected) value.
    proto::ProtoContext scope(parent->space, parent);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, readNextForm(&scope, macroTok));
    return scope.newList()
        ->appendLast(&scope, internNamed(&scope, named_, head))
        ->appendLast(&scope, scope.getAutomaticLocal(0))
        ->asObject(&scope);
}

namespace {

constexpr proto::proto_ulong kTagMask      = 0x3F;
constexpr proto::proto_ulong kTagList      = 2;   // POINTER_TAG_LIST
constexpr proto::proto_ulong kTagListSmall = 25;  // POINTER_TAG_LIST_SMALL

bool isListValue(const proto::ProtoObject* v) {
    const proto::proto_ulong tag = reinterpret_cast<proto::proto_ulong>(v) & kTagMask;
    return v != nullptr && (tag == kTagList || tag == kTagListSmall);
}

} // namespace

// Syntax-quote, as Clojure's reader expands it (LispReader.syntaxQuote):
//
//   `sym          (quote sym)
//   `:k `"s" `1   the value itself (keywords, strings, numbers, nil and
//                 booleans evaluate to themselves)
//   `~x           x
//   `~@x          a read error: a splice must be inside a list
//   `(a ~b ~@c)   (clojure.core/seq (clojure.core/concat
//                    (clojure.core/list (quote a)) (clojure.core/list b) c))
//   `()           (clojure.core/list)
//   `[a ~b]       (clojure.core/apply clojure.core/vector
//                    (clojure.core/seq (clojure.core/concat ...)))
//   `{k ~v}       (clojure.core/apply clojure.core/hash-map
//                    (clojure.core/seq (clojure.core/concat ...))),
//                 the entries in source order
//
// A nested syntax-quote has already been expanded when the outer one sees
// it, so its result is quoted as data, as in Clojure.
//
// Symbols, as Clojure's reader treats them: `foo#` is the generated symbol
// `foo__N__auto__`, the same one throughout this syntax-quote; any other
// symbol is qualified by the resolver (setSymbolResolver): a special form,
// `&` and an already qualified symbol stay as they are, a built-in becomes
// `clojure.core/x` and any other name `user/x` (compiler/Eval.h).
//
// `form` must be rooted by the caller; the result is unrooted.
const proto::ProtoObject* Reader::syntaxQuote(proto::ProtoContext* parent,
                                              const proto::ProtoObject* form,
                                              const Token& tok) {
    checkNativeStack(StackUse::Source);
    auto sym = [&](proto::ProtoContext* c, const char* name) {
        return internNamed(c, named_, name);
    };
    if (form == PROTO_NONE || form == PROTO_TRUE || form == PROTO_FALSE ||
        proto::ProtoObject::isStringTagFast(form) ||
        form->isInteger(parent) || form->isFloat(parent)) {
        return form;
    }
    if (isNamed(parent, named_, form)) {
        const std::string spelling = namedSpellingText(parent, named_, form);
        if (!spelling.empty() && spelling[0] == ':') return form;  // a keyword
        const proto::ProtoObject* symbol = form;
        if (spelling.size() > 1 && spelling.back() == '#' &&
            spelling.find('/') == std::string::npos) {
            // `foo#`: the same generated symbol everywhere in this
            // syntax-quote, `foo__N__auto__`.
            auto& table = gensyms_.back();
            auto it = table.find(spelling);
            if (it == table.end()) {
                const std::string fresh = spelling.substr(0, spelling.size() - 1) + "__" +
                                          std::to_string(nextGensymId()) + "__auto__";
                it = table.emplace(spelling, internNamed(parent, named_, fresh.c_str())).first;
            }
            symbol = it->second;
        } else if (resolver_) {
            const std::string resolved = resolver_(parent, spelling);
            if (resolved != spelling) symbol = internNamed(parent, named_, resolved.c_str());
        }
        return parent->newList()
            ->appendLast(parent, sym(parent, "quote"))
            ->appendLast(parent, symbol)
            ->asObject(parent);
    }

    // The items to splice together: a list's elements, a vector's, or a
    // map's keys and values in source order. Each is reachable from `form`.
    std::vector<const proto::ProtoObject*> items;
    const char* builder = nullptr;  // vector / hash-map: applied to the seq
    if (isListValue(form)) {
        const proto::ProtoList* lst = form->asList(parent);
        const proto::proto_ulong n = lst->getSize(parent);
        if (n > 0) {
            const proto::ProtoObject* head = lst->getAt(parent, 0);
            if (head == sym(parent, "clojure.core/unquote")) {
                if (n != 2) throw ReaderError("unquote (~) takes exactly one form", tok.line, tok.column);
                return lst->getAt(parent, 1);
            }
            if (head == sym(parent, "clojure.core/unquote-splicing")) {
                throw ReaderError("splice (~@) not in list", tok.line, tok.column);
            }
        } else {
            return parent->newList()
                ->appendLast(parent, sym(parent, "clojure.core/list"))
                ->asObject(parent);
        }
        for (proto::proto_ulong i = 0; i < n; ++i) items.push_back(lst->getAt(parent, static_cast<int>(i)));
    } else if (isVector(form)) {
        builder = "clojure.core/vector";
        const proto::ProtoList* lst = vectorItems(parent, form);
        const proto::proto_ulong n = lst->getSize(parent);
        for (proto::proto_ulong i = 0; i < n; ++i) items.push_back(lst->getAt(parent, static_cast<int>(i)));
    } else if (isMap(form)) {
        builder = "clojure.core/hash-map";
        const std::vector<const proto::ProtoObject*>* order = nullptr;
        if (info_) {
            auto it = info_->mapKeyOrder.find(form);
            if (it != info_->mapKeyOrder.end()) order = &it->second;
        }
        if (order) {
            for (const proto::ProtoObject* k : *order) {
                bool found = false;
                items.push_back(k);
                items.push_back(mapGet(parent, form, k, &found));
            }
        } else {
            mapForEach(parent, form, &items,
                [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
                   const proto::ProtoObject* v) {
                    auto* out = static_cast<std::vector<const proto::ProtoObject*>*>(self);
                    out->push_back(k);
                    out->push_back(v);
                });
        }
    } else if (isSet(parent, form)) {
        // A set template is applied to hash-set, as in Clojure; its elements
        // come in the set's (unspecified) order.
        builder = "clojure.core/hash-set";
        setForEach(parent, form, &items,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* e) {
                static_cast<std::vector<const proto::ProtoObject*>*>(self)->push_back(e);
            });
    } else {
        // Anything else the reader does not yield; quoted as data.
        return parent->newList()
            ->appendLast(parent, sym(parent, "quote"))
            ->appendLast(parent, form)
            ->asObject(parent);
    }

    // (clojure.core/concat piece...), each piece built in a slot of its own
    // scope chained to the builder's context, then parked by push.
    const proto::ProtoObject* unquote = sym(parent, "clojure.core/unquote");
    const proto::ProtoObject* splice = sym(parent, "clojure.core/unquote-splicing");
    const proto::ProtoObject* listSym = sym(parent, "clojure.core/list");
    proto::ProtoContext outer(parent->space, parent);
    outer.resizeAutomaticLocals(1);
    {
        ListBuilder concat(&outer);
        concat.push(sym(concat.context(), "clojure.core/concat"));
        for (const proto::ProtoObject* item : items) {
            if (isListValue(item)) {
                const proto::ProtoList* il = item->asList(concat.context());
                if (il->getSize(concat.context()) == 2 && il->getAt(concat.context(), 0) == splice) {
                    concat.push(il->getAt(concat.context(), 1));
                    continue;
                }
                if (il->getSize(concat.context()) == 2 && il->getAt(concat.context(), 0) == unquote) {
                    proto::ProtoContext pieceScope(concat.context()->space, concat.context());
                    concat.push(pieceScope.newList()
                        ->appendLast(&pieceScope, listSym)
                        ->appendLast(&pieceScope, il->getAt(&pieceScope, 1))
                        ->asObject(&pieceScope));
                    continue;
                }
            }
            proto::ProtoContext pieceScope(concat.context()->space, concat.context());
            pieceScope.resizeAutomaticLocals(1);
            pieceScope.setAutomaticLocal(0, syntaxQuote(&pieceScope, item, tok));
            concat.push(pieceScope.newList()
                ->appendLast(&pieceScope, listSym)
                ->appendLast(&pieceScope, pieceScope.getAutomaticLocal(0))
                ->asObject(&pieceScope));
        }
        outer.setAutomaticLocal(0, concat.finish());
    }
    // (clojure.core/seq (concat ...)), applied to the builder for a vector
    // or a map.
    const proto::ProtoObject* seqForm = outer.newList()
        ->appendLast(&outer, sym(&outer, "clojure.core/seq"))
        ->appendLast(&outer, outer.getAutomaticLocal(0))
        ->asObject(&outer);
    if (!builder) return seqForm;
    outer.setAutomaticLocal(0, seqForm);
    return outer.newList()
        ->appendLast(&outer, sym(&outer, "clojure.core/apply"))
        ->appendLast(&outer, sym(&outer, builder))
        ->appendLast(&outer, outer.getAutomaticLocal(0))
        ->asObject(&outer);
}

const proto::ProtoObject*
Reader::readList(proto::ProtoContext* parent, TokenKind closeKind,
                 int openLine, int openColumn, const char* unterminated) {
    // One read scope per recursive call, as a ListBuilder: it owns a child
    // context of `parent` that roots the in-progress list, and it folds the
    // elements in in chunks so a large literal — `[0 1 … 69999]` — does not
    // pin every intermediate version of the list until the form is closed.
    //
    // The builder's context is the innermost one on this thread while it is
    // open, so nested forms are read through `items.context()`; a context
    // chained to `parent` instead would unlink the builder from the thread's
    // root chain when it destructs (see ListBuilder.h).
    //
    // The returned ProtoObject* is not rooted by us. The caller's contract is
    // to put it in its OWN slot immediately — the protoCore calling
    // convention: returns are unrooted, the caller takes responsibility.
    ListBuilder items(parent);

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) {
            const char* what = "unterminated list — missing `)`";
            if (closeKind == TokenKind::RBracket) what = "unterminated vector — missing `]`";
            else if (closeKind == TokenKind::RBrace) what = "unterminated map — missing `}`";
            if (unterminated) what = unterminated;
            throw ReaderError(what, openLine, openColumn);
        }
        if (tok.kind == closeKind) {
            return items.finish();
        }
        if (tok.kind == TokenKind::HashUnderscore) {
            discardNextForm(items.context(), tok);
            continue;
        }
        if (tok.kind == TokenKind::RParen ||
            tok.kind == TokenKind::RBracket ||
            tok.kind == TokenKind::RBrace) {
            throw ReaderError(
                std::string("mismatched ") + tokenKindName(tok.kind),
                tok.line, tok.column);
        }

        // `push` parks the form in a GC-visible slot as its FIRST action,
        // before anything allocates, so the element cannot be reclaimed
        // between the read that produced it and the append that consumes it.
        items.push(readFromToken(items.context(), tok));
    }
}

} // namespace protoClojure
