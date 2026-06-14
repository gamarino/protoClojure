# Foreign-dispatch protocol design — `(count py-list)` just works

> **Purpose.** Decide the precise mechanism by which Clojure's core
> collection vocabulary (`count`, `seq`, `first`, `rest`, `get`,
> `nth`, `conj`, `assoc`, `contains?`) operates on foreign UMD
> objects — Python lists, JavaScript arrays, protoST collections —
> with **zero user effort**. The user writes `(count py-list)` and
> it works; no conversion required, no special syntax. This is the
> piece that makes the cross-runtime promise feel real.

**Date.** 2026-06-14.
**Status.** Approved. Implementation lands in phase 4 (UMD
providers), per the v0.1 roadmap.

---

## 1. The design principle

**Zero burden for the programmer.** That is the entire principle.
Concretely:

- The user writes ordinary Clojure code against ordinary Clojure
  vocabulary.
- The user does *not* convert at every cross-runtime call.
- The user does *not* learn a parallel API for foreign objects.
- The user does *not* think about which runtime owns which value.

If a Clojure programmer reads:

```clojure
(:require [py/numpy :as np])

(let [arr (np/array [1 2 3 4])]
  (count arr))
;; => 4
```

…they should be able to predict the outcome from prior knowledge of
Clojure alone. The `np/array` returns *some* protoCore object; the
fact that it came from Python is the implementation. `count` is
just `count`.

**Performance is secondary.** Each foreign-dispatched call pays
roughly one extra attribute walk vs the native equivalent. In
hot loops on foreign data, that adds up. The right answer in that
case is "convert once with `py->clj`, then iterate against native
Clojure" — a conscious decision the user makes when profiling.
We do not optimise the boundary by leaking it into the surface.

This is the protoCore-ecosystem principle ("purity > performance
until users justify otherwise") applied at the interop layer.

---

## 2. The mechanism

Clojure already has a polymorphism mechanism for "the same function
works on different types": **protocols**. We use exactly that
mechanism — no new machinery — and extend the relevant protocols
to foreign types at provider-registration time.

The picture in three lines:

1. `clojure.core` declares six protocols (`ICounted`, `ISeqable`,
   `IIndexed`, `ILookup`, `IAssociative`, `ICollection`).
2. Native Clojure types (vector, map, set, list, string, lazy seq)
   are extended to these protocols at `core.clj` load time.
3. Each foreign UMD provider (`py/`, `js/`, `pst/`) extends the
   same protocols to *its* native collection types when the
   provider registers — which happens the first time the user
   `(:require [py/...])` anything.

The result: `count`, `seq`, `get`, etc. are protocol functions that
dispatch on the receiver's type via the standard protoCore
`getAttribute` chain walk. The walk finds the implementation
regardless of which language created the value.

---

## 3. The six protocols

The minimum set that makes the core collection vocabulary work.
Names borrow Clojure-JVM and ClojureScript conventions for
familiarity.

### 3.1 `ICounted`

```clojure
(defprotocol ICounted
  "Things that know their own size."
  (-count [coll]))
```

Backs `count`, indirectly backs `empty?` and `bounded-count`.

### 3.2 `ISeqable`

```clojure
(defprotocol ISeqable
  "Things that can be walked as a seq of values."
  (-seq [coll]))
```

Backs `seq`, indirectly backs every higher-order function that
takes a `coll` argument (`map`, `filter`, `reduce`, `into`, …).

### 3.3 `IIndexed`

```clojure
(defprotocol IIndexed
  "Things addressable by integer position."
  (-nth [coll n] [coll n not-found]))
```

Backs `nth`. Required for vectors and arrays; *not* implemented by
maps, sets, or lists.

### 3.4 `ILookup`

```clojure
(defprotocol ILookup
  "Things addressable by arbitrary key."
  (-lookup [coll k] [coll k not-found]))
```

Backs `get`. Maps implement it with key→value; vectors implement
it with index→element; sets implement it with element→element or
nil; strings implement it with index→char.

### 3.5 `IAssociative`

```clojure
(defprotocol IAssociative
  "Things that support assoc and contains?."
  (-assoc      [coll k v])
  (-contains-key? [coll k]))
```

Backs `assoc`, `contains?`, indirectly `assoc-in`, `update`,
`update-in`, `merge`.

### 3.6 `ICollection`

```clojure
(defprotocol ICollection
  "Things that support conj."
  (-conj [coll x]))
```

Backs `conj`, indirectly `into`.

---

## 4. The dispatch chain — what happens at `(count x)`

```
(count x)
   │
   ▼
clojure.core/count          ; a defn that calls -count via the protocol
   │
   ▼
protocol dispatch           ; reads the -count attribute from x's
   │                        ; prototype chain via protoCore's
   │                        ; getAttribute chain walk
   ▼
the appropriate -count impl ; native, or the one a UMD provider
                            ; installed at provider-registration time
```

The protocol dispatch is **one attribute walk** — the same path
protoST uses for every message send, the same path protoJS uses
for property access. No special foreign-detection code, no
runtime-type branching, no marshalling. The kernel does the work.

Per-call cost: ~10ns for the attribute walk on a warm cache,
versus ~3ns for a direct C++ primitive call. The 7ns delta is the
"zero-burden tax" — paid once per protocol call, regardless of
which runtime created the receiver.

---

## 5. Bootstrap order

The trickier part. The protocols are *defined* in `core.clj`. The
native extensions happen at `core.clj` load. The foreign extensions
happen *lazily* at provider-registration time. Walk-through:

### 5.1 Step-by-step at startup

1. **protoCore initialises** — bootstrap prototypes installed.
2. **C++ primitives install** — `count` enters `clojure.core` as a
   C++-implemented function that *calls into a not-yet-defined
   protocol*. Specifically: `count` does
   `(satisfies? ICounted x) → (-count x)` or falls back to a generic
   `O(n)` walk. This shim handles the bootstrap window before
   `core.clj` loads.
3. **`core.clj` loads**:
   - The six `defprotocol` forms run. The protocols now exist as
     protoCore objects with the right shape (their method-name
     symbols are interned, their dispatch tables are empty).
   - The `extend-type` block for each native Clojure collection
     runs. After this, `(count [1 2 3])` and `(count {:a 1})`
     dispatch through the protocol.
   - `count` (the user-facing function) is re-defined in Clojure
     to *always* go through the protocol; the C++ shim falls out
     of use.
4. **The user starts their script.**

### 5.2 What happens on `(:require [py/numpy :as np])`

1. The Clojure compiler sees the `py/` prefix and looks up the
   protoPython provider in the UMD registry.
2. **If protoPython has not been initialised in this process yet**,
   the provider initialises it. Provider initialisation includes
   running the **foreign-extension block** (§6 below) — the
   `extend-type` calls that install the six protocols on every
   Python collection type the provider supports.
3. The provider then resolves `numpy` and returns its module
   object.
4. The Clojure compiler binds `np` and continues.

The user does not see any of this. From their perspective,
`(:require [py/...])` worked and now `count` works on Python
objects. The lazy provider-init is the design's only complication
and it is paid once per provider per process.

### 5.3 What about extensions across providers?

If the user has loaded `py/`, `js/`, and `pst/`, the protocols
have implementations for the Cartesian product of (six protocols)
× (every foreign type each provider supports). Each `extend-type`
costs ~constant work at registration and zero work per subsequent
call. The provider keeps a record of what it has registered, so a
repeated `(:require [py/numpy])` does not re-register.

---

## 6. Per-provider extension — the catalog

What each UMD provider extends to the six protocols. The pattern is
the same across providers; the *types* differ. Lengthy but
deterministic — each entry is a few lines of Clojure inside the
provider's bootstrap.

### 6.1 The `py/` provider (protoPython)

| Python type      | ICounted | ISeqable | IIndexed | ILookup | IAssociative | ICollection |
|------------------|----------|----------|----------|---------|--------------|-------------|
| `list`           | ✓ (`len`)| ✓ (`iter`)| ✓ (`[i]`)| ✓ (`[i]`)| —          | ✓ (`append`-style, returns new) |
| `tuple`          | ✓        | ✓        | ✓        | ✓       | —            | —           |
| `dict`           | ✓        | ✓ (items)| —        | ✓       | ✓            | ✓ (`{**d,k:v}`) |
| `set`            | ✓        | ✓        | —        | ✓ (mem) | —            | ✓ (set-conj) |
| `frozenset`      | ✓        | ✓        | —        | ✓       | —            | ✓           |
| `str`            | ✓        | ✓        | ✓        | ✓       | —            | —           |
| `range`          | ✓        | ✓        | ✓        | —       | —            | —           |
| `bytes`          | ✓        | ✓        | ✓        | ✓       | —            | —           |

A subtle point: **`conj` on a foreign mutable collection returns a
new collection, not mutates the original.** This is Clojure's
contract; the provider's `extend-type` block honours it by copying
on conj. For Python lists, this means `(conj py-list x)` creates a
new list with the element appended. The original `py-list` is
unchanged. This *does* cost an `O(n)` copy — explicitly. If the
user wants the Python mutable semantics, they call `(py/.append
py-list x)` directly through the foreign API.

### 6.2 The `js/` provider (protoJS)

| JS type          | ICounted | ISeqable | IIndexed | ILookup | IAssociative | ICollection |
|------------------|----------|----------|----------|---------|--------------|-------------|
| `Array`          | ✓ (`length`)| ✓     | ✓ (`[i]`)| ✓ (`[i]`)| —          | ✓ (copy + push) |
| Plain `Object`   | ✓ (keys) | ✓ (entries)| —      | ✓       | ✓            | ✓ ({...o,k:v}) |
| `Map`            | ✓ (`size`)| ✓      | —        | ✓       | ✓            | ✓           |
| `Set`            | ✓        | ✓        | —        | ✓ (has) | —            | ✓           |
| `String`         | ✓        | ✓        | ✓        | ✓       | —            | —           |
| `Int8Array` &c.  | ✓        | ✓        | ✓        | ✓       | —            | —           |

JS distinguishes "plain Object" from a `Map`. The former is the
JS idiom for a string-keyed dict; we treat it like a Clojure map
with string keys.

### 6.3 The `pst/` provider (protoST)

| protoST type        | ICounted | ISeqable | IIndexed | ILookup | IAssociative | ICollection |
|---------------------|----------|----------|----------|---------|--------------|-------------|
| `Array`             | ✓        | ✓        | ✓        | ✓       | —            | ✓           |
| `OrderedCollection` | ✓        | ✓        | ✓        | ✓       | —            | ✓           |
| `Dictionary`        | ✓        | ✓        | —        | ✓       | ✓            | ✓           |
| `Set`               | ✓        | ✓        | —        | ✓       | —            | ✓           |
| `Bag`               | ✓        | ✓        | —        | —       | —            | ✓           |
| `String`            | ✓        | ✓        | ✓        | ✓       | —            | —           |
| `Interval`          | ✓        | ✓        | ✓        | —       | —            | —           |

The protoST collections are already protoCore-native — they live
in the same memory space and the same prototype graph as
everything else. The "conversion" for ICounted is `Array
>> size`, directly callable.

### 6.4 Implementation surface

Per provider, the extension block is **~40-80 lines of Clojure**
(roughly, six lines per protocol × six-to-eight types). Three
providers = ~200 lines of Clojure total, lazily loaded.

The C++ side of the provider needs to know **when** to run the
extension block — at first protoClojure use of the provider in the
process. The hook is `UMDProvider::onFirstClojureUse(callback)`
registered at provider construction. The callback evaluates the
provider's extension `.clj` file. Once.

---

## 7. What about types not extended?

If the user calls `(count foreign-thing)` and `foreign-thing`'s
type is **not** in any of the protocol tables, the dispatch raises
a clear error:

```
ProtocolNotExtended: No implementation of ICounted for type
'pandas.DataFrame'. Possible fixes:

  1. Use the foreign type's own API:
       (py/.shape df)              ; for shape (rows, cols)
       (py/len df)                  ; for row count

  2. Extend the protocol yourself if the type supports it:
       (extend-type pandas.DataFrame
         ICounted
         (-count [df] (py/.size (py/.index df))))

  3. Convert with py->clj if applicable:
       (count (py->clj some-foreign-coll))

  See docs/INTEROP.md §3 for the full conversion rules.
```

Three things the message does:

1. Names the type precisely so the user knows what is missing.
2. Offers three concrete fixes in the order of cheapness.
3. Points at the docs section that covers the general case.

The error message is the difference between an honest gap and a
mystery. Make it good.

---

## 8. The conversion functions — when to reach for them

The protocols cover the **common** collection operations. They do
not cover everything. The boundary stays explicit for:

- **Identity equality across types.** `(= py-list clj-vec)` returns
  false in v0.1, even when the contents are identical — see §10.
- **Hash codes for mixed-runtime maps/sets.** Cross-runtime
  membership is not v0.1 (see §10).
- **Type predicates.** `(vector? py-list)` is **false** — a Python
  list is not a vector. `(sequential? py-list)` is **true** —
  sequential is an open protocol the providers extend. The line
  between concrete type predicates (`vector?`) and behaviour
  protocols (`sequential?`) is the same line Clojure-JVM draws.
- **Specific destination shape**. NumPy wants an `ndarray`, not a
  generic seq. Pandas wants a `DataFrame`. D3 wants a JS Array of
  plain Objects. For these, the explicit `clj->py` / `clj->js`
  conversion is the right tool.

The user's mental model: protocols handle "read me as a Clojure
collection"; conversion handles "give me a native X for this
foreign API to consume." The first is zero-burden. The second is
named and visible.

---

## 9. Performance characteristics

Honest accounting.

### 9.1 Cost per call

| Operation                           | Native cost       | Foreign cost      | Delta            |
|-------------------------------------|-------------------|-------------------|------------------|
| `(count clj-vec)`                   | ~3ns (one C call) | (same)            | 0                |
| `(count py-list)`                   | ~3ns (native shim)| ~10ns (attr walk + Python `len`) | +7ns |
| `(first py-list)`                   | n/a               | ~12ns             | small            |
| `(nth py-list 5)`                   | n/a               | ~12ns             | small            |
| `(reduce + py-list)` (length 100)   | n/a               | ~1.4µs            | +700ns vs native (~7ns × 100 calls) |
| `(reduce + py-list)` (length 10000) | n/a               | ~140µs            | +70µs vs native  |

For collections under a few hundred elements, the overhead is in
the noise. For larger collections in inner loops, conversion (with
`py->clj`) pays itself back in a few iterations.

### 9.2 Strategies the user has when this matters

1. **Convert once, iterate against native:**

   ```clojure
   (let [vs (py->clj py-list)]
     (reduce + vs))      ;; native fast path
   ```

2. **Push the loop into the foreign runtime:**

   ```clojure
   (np/sum py-array)     ;; NumPy's vectorised loop, in C
   ```

3. **Hold the foreign object as opaque, operate via foreign API:**

   ```clojure
   (np/.shape arr)       ;; talk to NumPy, never lift to Clojure
   ```

The user picks the strategy when they profile and find a hot
loop. The default — protocol dispatch — does the right thing
without thought.

### 9.3 Future optimisation directions (not v0.1)

- **Inline caching at the call site**: cache the last-resolved
  protocol implementation per call-site bytecode. Like the protoST
  inline cache. Reduces foreign-call cost to ~3-4ns on the hot
  path.
- **Type-specific fast paths in C++** for the most common cases
  (`(count py-list)`, `(count js-array)`): a one-line C++ branch
  that recognises the exact type and calls the foreign primitive
  directly, skipping the attribute walk.
- **A "this loop is foreign" annotation** the user opts into to
  switch a region of code to foreign-dispatch-optimised mode.

All of these are post-v0.1. v0.1 ships the clean version and
documents it.

---

## 10. Equality across runtimes — the deliberate non-decision

A subtle question: should `(= [1 2 3] py-list-of-1-2-3)` return
true?

**v0.1 answer: false.** Cross-runtime structural equality is **not**
implemented. To compare across the boundary, convert first.

```clojure
(= [1 2 3] (py->clj py-list))  ;; works
(= [1 2 3] py-list)             ;; false, no error
```

**Reasoning:**

- The same value can be present in *both* sides of a `=` and still
  fail it without conversion, which is *not* how Clojurists
  intuit `=`. That is a real surprise.
- The alternative — make `=` true across the boundary — opens a
  pandora's box: now sets and maps with mixed Clojure/foreign keys
  need cross-runtime hashing, which is a much larger design.
- The conversion is one function call and makes the boundary
  visible, which is the principle this whole document is built
  on.

v0.x may revisit if user feedback says this surprises too much in
practice. For v0.1, the explicit-conversion route is sound.

The same logic applies to `(contains? clj-set py-element)` —
returns false unless the element is converted. Document.

---

## 11. Mutability at the boundary

Another subtle area. A Python list is mutable; a Clojure vector
is not. If the user holds a `py-list` and operates on it through
the Clojure protocols:

```clojure
(def py-list (py/list [1 2 3]))   ;; Python's [1, 2, 3]

(count py-list)                    ;; => 3
(py/.append py-list 4)             ;; mutates py-list directly
(count py-list)                    ;; => 4  ← changed!
```

This is **expected** in v0.1. The Clojure protocols give the user a
*view*, not a snapshot. If the foreign code mutates the underlying
object, subsequent Clojure reads see the change.

The zero-burden principle accepts this — it is the price of "no
conversion required, no marshalling cost." The user who wants
snapshot semantics calls `(py->clj py-list)` to get a real Clojure
vector that does not move.

Document this. It is the only place in the design where the user
has to remember "this thing came from Python".

---

## 12. The protocol declarations in `core.clj`

Concrete code. This is roughly what `core.clj` adds for the v0.1
foreign-dispatch layer (about 200 lines for both protocol
declarations and native extensions):

```clojure
;; --- Protocols ---

(defprotocol ICounted
  "Things that know their own size in O(1)."
  (-count [coll]))

(defprotocol ISeqable
  "Things that can be walked as a sequence of values."
  (-seq [coll]))

(defprotocol IIndexed
  "Things addressable by integer position."
  (-nth [coll n] [coll n not-found]))

(defprotocol ILookup
  "Things addressable by arbitrary key."
  (-lookup [coll k] [coll k not-found]))

(defprotocol IAssociative
  "Things that support assoc and contains?."
  (-assoc          [coll k v])
  (-contains-key?  [coll k]))

(defprotocol ICollection
  "Things that support conj."
  (-conj [coll x]))

;; --- Native Clojure types ---

(extend-type ::vector
  ICounted     (-count       [v]     (clj-vector-count v))
  ISeqable     (-seq         [v]     (clj-vector-seq v))
  IIndexed     (-nth         ([v n]            (clj-vector-nth v n))
                              ([v n not-found]  (clj-vector-nth v n not-found)))
  ILookup      (-lookup      ([v k]            (clj-vector-nth v k nil))
                              ([v k not-found]  (clj-vector-nth v k not-found)))
  IAssociative (-assoc       [v k val] (clj-vector-assoc v k val))
               (-contains-key? [v k]   (and (integer? k) (< -1 k (count v))))
  ICollection  (-conj        [v x]    (clj-vector-conj v x)))

(extend-type ::map ... )
(extend-type ::set ... )
(extend-type ::list ... )
(extend-type ::string ... )
(extend-type ::lazy-seq ... )

;; --- The user-facing functions go through the protocols ---

(defn count [coll]
  (if (nil? coll) 0 (-count coll)))

(defn seq [coll]
  (when-not (nil? coll) (-seq coll)))

(defn first [coll]
  (when-let [s (seq coll)] (-first s)))

(defn rest [coll]
  (if-let [s (seq coll)] (-rest s) '()))

(defn nth
  ([coll n]            (-nth coll n))
  ([coll n not-found]  (-nth coll n not-found)))

(defn get
  ([coll k]            (-lookup coll k nil))
  ([coll k not-found]  (-lookup coll k not-found)))

(defn assoc [coll k v & kvs]
  (let [c (-assoc coll k v)]
    (if kvs
      (apply assoc c kvs)
      c)))

(defn contains? [coll k] (-contains-key? coll k))

(defn conj
  ([] [])
  ([coll] coll)
  ([coll x] (-conj coll x))
  ([coll x & more] (apply conj (-conj coll x) more)))
```

The `clj-vector-*` primitives in the `extend-type` blocks are
the C++ functions from the clojure.core spec §3.3 — but renamed
with the `clj-` prefix to denote "native fast path", separate
from the user-facing protocol function. The user calls `count`,
which calls `-count`, which dispatches to (for a native vector)
`clj-vector-count`. One protocol hop, one C++ call.

---

## 13. The provider extension blocks — concrete

The `py/` provider's extension block, illustrative:

```clojure
;; resources/clojure/provider/py.clj
;; Loaded by the protoPython UMD provider at first use.

(ns clojure.provider.py
  (:require [clojure.core
             :refer [ICounted ISeqable IIndexed ILookup IAssociative ICollection
                     extend-type defn nil?]]))

(extend-type py/list
  ICounted    (-count    [x] (py/len x))
  ISeqable    (-seq      [x] (py->seq x))
  IIndexed    (-nth      ([x n]            (py/getitem x n))
                          ([x n not-found]  (try (py/getitem x n)
                                                 (catch py/IndexError _ not-found))))
  ILookup     (-lookup   ([x k]            (-nth x k nil))
                          ([x k not-found]  (-nth x k not-found)))
  ICollection (-conj     [x v]
                         ;; conj on a Python list returns a NEW list, per Clojure contract.
                         ;; This is O(n) — the cost of zero-burden semantics on a mutable type.
                         (let [c (py/list x)]
                           (py/.append c v)
                           c)))

(extend-type py/dict
  ICounted     (-count          [d]     (py/len d))
  ISeqable     (-seq            [d]     (py->seq (py/.items d)))
  ILookup      (-lookup         ([d k]            (py/getitem d k))
                                 ([d k not-found]  (py/.get d k not-found)))
  IAssociative (-assoc          [d k v] (let [c (py/dict d)] (py/setitem c k v) c))
                (-contains-key? [d k]   (py/contains d k))
  ICollection  (-conj           [d kv]
                                (when (or (nil? kv) (not= 2 (count kv)))
                                  (throw (ex-info "conj on a dict expects a [k v] pair" {:got kv})))
                                (-assoc d (first kv) (last kv))))

;; ... tuple, set, frozenset, str, range, bytes ...
```

The `js/` and `pst/` extension blocks follow the same pattern with
the language's own native type names. Total: ~200 lines of
Clojure, shipped with the runtime, loaded lazily.

---

## 14. Implementation checklist for phase 4

When phase 4 (UMD providers) begins, this is the work plan:

1. **C++**: implement the `UMDProvider` registry and the
   `onFirstClojureUse(callback)` hook.
2. **C++**: wire the protoPython, protoJS, and protoST UMD bridges
   to register themselves with their provider tag.
3. **Clojure**: write the six `defprotocol` forms and the
   `extend-type` blocks for native Clojure collections in
   `core.clj`. Loads at startup.
4. **Clojure**: write `clojure/provider/py.clj`,
   `clojure/provider/js.clj`, `clojure/provider/pst.clj`. Each
   provider's bootstrap evaluates its `.clj` extension file once,
   the first time the provider is needed.
5. **Tests**: per provider, a conformance fixture that exercises
   each protocol on each foreign type. ~6 protocols × ~6 types
   per provider × 3 providers = ~100 fixtures total.
6. **Error path**: implement the rich error message from §7. One
   shared formatter, called from each protocol's fallback path.
7. **Docs**: update `INTEROP.md` §3.2 to point at this spec for
   the per-type catalog, since the doc and the spec will drift
   if the catalog only lives in the spec.

The work breakdown for phase 4 stays at the 2-week target from
`ROADMAP.md`. Most of the cost is the provider bridges, not the
protocol extensions, which are small.

---

## 15. Open questions for after v0.1

Things we know we are deferring and the rough direction.

- **Inline caching at SEND_CALL-like sites** for protocol dispatch.
  Same principle as protoST's call-form dispatch caching.
- **Type-specific fast paths in C++** for the most common foreign
  type pairs (`Python list`, `JS Array`).
- **Cross-runtime structural equality** via `IEquiv` extension to
  foreign types. Subtle on hash sets/maps.
- **`pmap` cooperation** with foreign types: does
  `(pmap f py-list)` parallelise well, given the GIL is in
  protoCore but not in Python's GIL (the protoPython runtime
  removed it)? Should be measured.
- **Foreign types as map keys.** `(get {py-key v} py-key)` —
  requires consistent cross-runtime hash. Reserved.

---

## 16. Definition of done

The foreign-dispatch design is **done in phase 4** when:

1. The six protocols exist in `core.clj` with native extensions
   for vector/map/set/list/string/lazy-seq.
2. Each of the three providers (`py/`, `js/`, `pst/`) ships an
   extension block covering the types in §6.
3. The user-facing functions (`count`, `seq`, `first`, `rest`,
   `nth`, `get`, `assoc`, `contains?`, `conj`, `into`, `reduce`)
   all dispatch through the protocols.
4. `(count py-list)`, `(count js-array)`, `(count pst-array)`,
   and the analogous calls for every protocol×type pair in §6
   return the right value, on every supported type.
5. The error message from §7 fires with the format shown when an
   unextended type meets a protocol.
6. The mutability note from §11 is documented in
   `INTEROP.md`.
7. The performance characteristics in §9 are measured and the
   numbers either match or the spec is updated.

---

## 17. Revisions

(empty for now)
