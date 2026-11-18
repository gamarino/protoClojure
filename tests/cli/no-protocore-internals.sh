#!/usr/bin/env bash
#
# Source check: protoClojure depends on protoCore's public API only.
#
# The runtime must never include protoCore's private header, name protoCore's
# pointer tags or embedded types, or hand-copy their numeric values (for
# example `(p & 0x3F) == 15` for a Double). Every type test goes through
# headers/protoCore.h (ProtoObject::is*/as*, isSmallInt, getPrototype, ...).
# This check fails if a source file under src/ or headers/ reintroduces any of
# those dependencies.
#
# Usage: no-protocore-internals.sh <repository-root>
set -u
ROOT="${1:?usage: no-protocore-internals.sh <repository-root>}"

dirs=()
for d in src headers; do
    [[ -d "$ROOT/$d" ]] && dirs+=("$ROOT/$d")
done

# 1. protoCore's private header.
# 2. protoCore's internal tag and embedded-type names.
# 3. Hand-copied tag constants (kTag..., k...PointerTag).
# 4. A pointer value masked with protoCore's 6-bit tag or 10-bit tag+type mask.
patterns=(
    'proto_internal\.h'
    '(POINTER_TAG|EMBEDDED_TYPE)'
    '\bkTag[A-Z][A-Za-z0-9_]*|[A-Za-z0-9_]*PointerTag\b'
    '(reinterpret_cast<[^>]*(uintptr_t|proto_ulong|uint64_t|unsigned long)[^>]*>|\(uintptr_t\)|\(proto_ulong\)).*&[[:space:]]*(PROTO_UL\()?0x3FF?\b'
)

status=0
for pat in "${patterns[@]}"; do
    hits=$(grep -rnE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.cc' \
                "$pat" "${dirs[@]}" 2>/dev/null)
    if [[ -n "$hits" ]]; then
        echo "FAIL: protoCore internals referenced (pattern: $pat):"
        echo "$hits"
        status=1
    fi
done

if [[ $status -eq 0 ]]; then
    echo "OK: no protoCore internals referenced under ${dirs[*]}"
fi
exit $status
