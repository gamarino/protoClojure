#pragma once

// Value-kind tests built from protoCore's public API only (headers/
// protoCore.h). protoClojure never includes protoCore's private header and
// never copies its pointer tags, embedded types or cell layouts; the CTest
// case source/no-protocore-internals enforces it.
//
// Two documented facts of the public API carry the compositions below:
//  - ProtoObject::getPrototype answers, for every value that is not an
//    object cell, the ProtoSpace prototype of its kind (listPrototype,
//    mapPrototype, sparseListPrototype, getLinkedListPrototype(), ...); for
//    an object cell it answers the object's first parent, or
//    objectPrototype when it has none. Several kinds' prototypes are
//    aliases of objectPrototype (see isAliasedCellKind), so prototype
//    identity is used only for kinds with a prototype of their own.
//  - ProtoObject::getFirstParent answers PROTO_NONE for every value that is
//    not an object cell, and for a parentless object.
//
// The ProtoObject::is*/as* predicates for collections (isMap, isTuple,
// isLinkedList, asList, ...) are deliberately not used for kind tests: they
// also accept an object cell that wraps such a value under `__data__` (and
// asList accepts strings), which is not what "is this value a ProtoMap"
// means here, and they probe an attribute on every object cell.

#include "protoCore.h"

namespace protoClojure {

// True when `v` is a value of the protoCore kind whose public prototype is
// `kindProto`, and not an object cell that merely inherits from it. One
// out-of-line call for every value whose kind differs.
inline bool isPrimitiveOfKind(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                              const proto::ProtoObject* kindProto) {
    if (!v || v->getPrototype(ctx) != kindProto) return false;
    return !proto::ProtoObject::isCellPointer(v) || v->getFirstParent(ctx) == PROTO_NONE;
}

// protoCore leaves the ProtoSpace prototype of several cell kinds aliased to
// objectPrototype (LargeIntegers, doubles, methods, byte buffers, threads,
// external pointers and buffers), so getPrototype cannot tell such a value
// from a parentless object. The public tag-only predicates do.
inline bool isAliasedCellKind(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return v->isDouble(ctx) || v->isInteger(ctx) || v->isMethod(ctx) || v->isByteBuffer(ctx) ||
           v->asThread(ctx) != nullptr || v->asExternalPointer(ctx) != nullptr ||
           v->asExternalBuffer(ctx) != nullptr;
}

// True when `v` is a plain protoCore object cell: maps of attributes such as
// keywords, symbols, atoms, functions, futures, promises, actors, sets and
// seq nodes. False for every number, string, collection and method.
inline bool isObjectCell(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    // Inline rejects first; then the common case, an object with a parent,
    // is decided by one call.
    if (!v || proto::isSmallInt(v) || proto::ProtoObject::isStringTagFast(v)) return false;
    if (v->getFirstParent(ctx) != PROTO_NONE) return true;
    if (!proto::ProtoObject::isCellPointer(v)) return false;
    const proto::ProtoObject* p = v->getPrototype(ctx);
    if (p != nullptr && p != ctx->space->objectPrototype) return false;
    return !isAliasedCellKind(ctx, v);
}

// A protoCore ProtoList, in either of its storage forms. Not a string, which
// ProtoObject::asList also answers for with its character list.
inline bool isProtoList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isPrimitiveOfKind(ctx, v, ctx->space->listPrototype);
}

// A protoCore ProtoMap, in either of its storage forms.
inline bool isProtoMap(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isPrimitiveOfKind(ctx, v, ctx->space->mapPrototype);
}

// A protoCore ProtoSparseList, in either of its storage forms.
inline bool isProtoSparseList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isPrimitiveOfKind(ctx, v, ctx->space->sparseListPrototype);
}

// A protoCore ProtoLinkedList, empty or not.
inline bool isProtoLinkedList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isPrimitiveOfKind(ctx, v, ctx->space->getLinkedListPrototype());
}

// A double (ProtoObject::isDouble is a tag-only test).
inline bool isDoubleValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return v && v->isDouble(ctx);
}

// A LargeInteger: an integer that is not a SmallInteger.
inline bool isLargeIntegerValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return v && !proto::isSmallInt(v) && v->isInteger(ctx);
}

// A protoCore method value: every primitive (ProtoObject::isMethod is a
// tag-only test).
inline bool isMethodValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return v && v->isMethod(ctx);
}

} // namespace protoClojure
