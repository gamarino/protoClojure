/*
 * CorePrims — the scripting vocabulary of Part C written in C++ (quot, rem,
 * mod, compare, max, min, sort, sort-by, frequencies, split-lines, require
 * and the `System` object); see CorePrims.cpp and docs/DESIGN.md §10.2.
 */
#pragma once

#include "protoCore.h"

namespace protoClojure {

// Installs them on the globals; called by installPrimitives after the I/O
// globals, which `System` reads.
void installCorePrimitives(proto::ProtoContext* ctx, proto::ProtoObject* globals);
// The name of one of them, for printing a fn; nullptr otherwise.
const char* corePrimitiveName(proto::ProtoMethod fn);

} // namespace protoClojure
