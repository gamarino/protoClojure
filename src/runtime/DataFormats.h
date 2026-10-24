/*
 * DataFormats — reading and writing data as text (macros brief, Part B stage
 * 12; docs/DESIGN.md §10.2, M35-M38):
 *
 *   - `read-string` (clojure.core): the reader on a string, as code is read
 *     (syntax-quote included), returning the first form (Edn.cpp);
 *   - `clojure.edn/read-string`: the same reader in EDN mode, data only: no
 *     evaluation, no `#=`, no code-only reader macros, no tagged literals
 *     (Edn.cpp);
 *   - JSON (Json.cpp), one C++ parser and one writer behind two APIs:
 *       cheshire.core      parse-string, parse-stream, generate-string,
 *                          generate-stream, decode, encode;
 *       clojure.data.json  read-str, read, write-str, write.
 *
 * `clojure.edn`, `cheshire.core` and `clojure.data.json` are global objects
 * whose members are the functions (as `System` is, CorePrims.cpp): the
 * compiler reads `cheshire.core/parse-string`, and `json/parse-string` once
 * `[cheshire.core :as json]` is required, as a member call (M37).
 */
#pragma once

#include "protoCore.h"

namespace protoClojure {

// Installs `read-string` and the `clojure.edn` object on `globals`.
void installEdn(proto::ProtoContext* ctx, proto::ProtoObject* globals);

// Installs the `cheshire.core` and `clojure.data.json` objects on `globals`.
void installJson(proto::ProtoContext* ctx, proto::ProtoObject* globals);

// The name a data-format function prints as (`#<fn parse-string>`), or
// nullptr when `fn` is none of them.
const char* dataFormatPrimitiveName(proto::ProtoMethod fn);

} // namespace protoClojure
