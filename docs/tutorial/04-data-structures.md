# 4. Data Structures

protoClojure inherits Clojure's four primary collection types — list,
vector, map, set — and adds the substrate's twist: every collection is
backed by a protoCore primitive that other languages on the kernel
share. A Clojure vector is *the same kind of object* a Python script or
a JavaScript program would touch if they reached into the same data via
the UMD bridge. That is not a coincidence; it is the design.

## 4.1 The four collections at a glance

| Collection | Literal           | Order      | Duplicates | Random access | Idiomatic for                       |
|------------|-------------------|------------|------------|---------------|-------------------------------------|
| List       | `'(1 2 3)`        | sequential | yes        | `O(n)`        | code-as-data, head-add accumulators |
| Vector     | `[1 2 3]`         | sequential | yes        | `O(log n)`    | the workhorse — almost everything   |
| Map        | `{:a 1 :b 2}`     | unordered  | keys no    | `O(log n)`    | structured records, lookups         |
| Set        | `#{1 2 3}`        | unordered  | no         | `O(log n)`    | membership, deduplication           |

The fast version of "which one do I reach for?":

- **Default to vectors.** A vector is the right answer 80% of the time.
- **Reach for a map when keys carry meaning.** `{:name "alice" :age 30}`
  is a record. `{"alice" 30 "bob" 25}` is an index.
- **Reach for a set when membership is the question.** `(contains? s x)`
  on a set is `O(log n)`; on a vector it is `O(n)`.
- **Reach for a list rarely.** Code-as-data is the main case. Most
  Clojure programmers write vector literals all day and write list
  literals only in macros.

## 4.2 Persistence — the "every modification returns a new value" rule

This is the most important property of every collection in this
chapter. From [Chapter 2](02-for-the-python-or-javascript-developer.md)
if you came from Python or JS — `(conj v x)` does not mutate `v`. It
returns a new vector. From [Chapter 3](03-for-the-clojure-developer.md)
if you came from Clojure-JVM — same as JVM.

```clojure
(def xs [1 2 3])
(def ys (conj xs 4))

xs                       ;; => [1 2 3]
ys                       ;; => [1 2 3 4]
```

Every operation we cover in this chapter follows that rule. There are
**no in-place mutators** in the collection API. The substrate makes
them cheap — protoCore collections share structure through AVL nodes
and hash-array-mapped tries — so the cost is `O(log n)` per
"modification", not `O(n)`. In practice the difference is invisible
on real workloads.

The one tool for genuinely mutable state is the `atom` (Chapter 6),
which is a *deliberate* tool, not a default.

## 4.3 Vectors

The everyday collection. Constructed with the literal `[...]` or with
`vector`:

```clojure
[1 2 3]
(vector 1 2 3)            ;; ≡ [1 2 3]
(vec '(1 2 3))             ;; converts a seq to a vector
```

Access is by index, with `nth` or by *calling the vector*:

```clojure
(nth [10 20 30] 1)         ;; => 20
([10 20 30] 1)             ;; => 20  — vectors are functions of their indices
([10 20 30] 5 :not-found)  ;; => :not-found  — with default
```

The core operations:

```clojure
(conj [1 2 3] 4)           ;; => [1 2 3 4]    — adds at tail
(conj [1 2 3] 4 5)         ;; => [1 2 3 4 5]  — multi-arg variant
(assoc [10 20 30] 1 99)    ;; => [10 99 30]   — replaces by index
(pop [1 2 3])              ;; => [1 2]        — removes from tail
(peek [1 2 3])              ;; => 3            — last element
(count [1 2 3])            ;; => 3
(empty? [])                ;; => true
(first [1 2 3])            ;; => 1
(rest [1 2 3])             ;; => (2 3)        — note: seq, not vector
(last [1 2 3])             ;; => 3
(reverse [1 2 3])          ;; => (3 2 1)      — seq
(vec (reverse [1 2 3]))    ;; => [3 2 1]      — back to vector
```

**Performance feel.** A vector is a 32-way trie. Up to 32 elements it
fits in one node. Past 32 you grow another level (one extra hop).
`conj` at the tail is amortised `O(1)`. `assoc` on an existing index
walks `log32 n` levels. For collections under 10,000 elements you can
treat operations as constant time and not be wrong.

## 4.4 Maps

Key→value pairs. Constructed with the literal `{...}` or with
`hash-map`:

```clojure
{:name "alice" :age 30}
(hash-map :name "alice" :age 30)
(zipmap [:a :b :c] [1 2 3])   ;; => {:a 1, :b 2, :c 3}
```

Access through `get`, by *calling the map*, or by *calling the keyword*:

```clojure
(get {:a 1 :b 2} :a)           ;; => 1
({:a 1 :b 2} :a)               ;; => 1   — maps are functions of keys
(:a {:a 1 :b 2})               ;; => 1   — keywords are functions of maps
(:c {:a 1 :b 2} :default)      ;; => :default  — with default
```

The idiom is overwhelmingly `(:key m)`, because it reads as "the key of
the map" — natural English order.

The core operations:

```clojure
(assoc {:a 1} :b 2 :c 3)       ;; => {:a 1, :b 2, :c 3}
(dissoc {:a 1 :b 2} :a)        ;; => {:b 2}
(update {:n 1} :n inc)         ;; => {:n 2}
(update {:n 1} :n + 10)        ;; => {:n 11}     — extra args after the fn
(merge {:a 1 :b 2} {:b 20 :c 3})
                                ;; => {:a 1, :b 20, :c 3}  — right wins
(select-keys {:a 1 :b 2 :c 3} [:a :c])
                                ;; => {:a 1, :c 3}
(keys {:a 1 :b 2})             ;; => (:a :b)
(vals {:a 1 :b 2})             ;; => (1 2)
(count {:a 1 :b 2})            ;; => 2
```

Nested access and update — the deep-`*-in` family:

```clojure
(def user {:name "alice"
           :address {:city "Buenos Aires"
                     :zip 1428}})

(get-in user [:address :city])           ;; => "Buenos Aires"
(assoc-in user [:address :zip] 9999)     ;; => new user with zip changed
(update-in user [:address :city] str/upper-case)
                                          ;; => new user with city upper-cased
```

These are how you "modify deeply nested data" in Clojure without
mutating anything.

## 4.5 Sets

Unordered, deduplicated. Constructed with `#{...}` or `hash-set`:

```clojure
#{:a :b :c}
(hash-set :a :b :c)
(set [1 1 2 2 3 3])            ;; => #{1 2 3}     — dedup a vector
```

A set is *a function of its elements*: calling it with a value returns
the value if present, `nil` otherwise. That is the membership test:

```clojure
(#{:a :b :c} :a)               ;; => :a
(#{:a :b :c} :z)               ;; => nil

(contains? #{:a :b} :a)        ;; => true
```

Core operations:

```clojure
(conj #{:a :b} :c)             ;; => #{:a :b :c}
(disj #{:a :b :c} :a)          ;; => #{:b :c}
(into #{} [1 2 2 3])           ;; => #{1 2 3}     — generic dedup pattern
(count #{1 2 3})               ;; => 3
```

Set algebra lives in `clojure.set` (a v0.2 deliverable; not in v0.1
core): `union`, `intersection`, `difference`. In v0.1, write them by
hand with `filter` and `into`.

## 4.6 Lists

The Lisp original. Singly-linked, cheap at the head, expensive in the
middle. You will use lists in two situations:

- **Code is data.** Every form the reader produces from `(...)` is a
  list. Most of the time you do not see them directly; macros do.
- **As an accumulator at the head.** Building a result by `(cons x
  acc)` is cheap because lists grow at the head.

```clojure
'(1 2 3)                       ;; literal list (quoted)
(list 1 2 3)                   ;; constructor
(cons 0 '(1 2 3))              ;; => (0 1 2 3)    — add at head
(first '(1 2 3))               ;; => 1
(rest '(1 2 3))                ;; => (2 3)
```

Notice the **quote**: `(1 2 3)` without the quote is a function call to
`1`, which fails. `'(1 2 3)` is the literal list. Vectors and maps and
sets do not need quoting because their literals are not also call forms.

For data, prefer vectors. Reserve lists for the cases where they make
sense.

## 4.7 The `seq` abstraction — what `(rest ...)` returns

You may have noticed:

```clojure
(rest [1 2 3])                 ;; => (2 3)   — looks like a list, not a vector
```

Many collection operations return a **sequence** (`seq`), which is a
shared abstraction Clojure uses for "an ordered, possibly-lazy walk over
something". Lists are seqs. Vectors, maps, sets and strings can all be
*seq'd over* (`(seq [1 2 3])`). Higher-order functions like `map`,
`filter`, `take`, and `range` all return seqs.

This matters mostly because:

- A seq prints as `(...)`, like a list, regardless of the source.
- Some operations are *lazy* and may not produce values until you ask
  (see Chapter 5 §5.8 for the consequences).
- If you specifically want the result back as a vector / map / set,
  wrap with `vec`, `into {}`, `into #{}`:

```clojure
(vec (map inc [1 2 3]))               ;; => [2 3 4]
(into {} (map (fn [[k v]] [k (inc v)]) {:a 1 :b 2}))
                                       ;; => {:a 2, :b 3}
(into #{} (filter odd? [1 2 3 4 5]))  ;; => #{1 3 5}
```

The `into` idiom is everywhere. Remember it.

## 4.8 Equality across collections

`=` is structural value equality. Two collections are equal iff their
contents are structurally equal — including across types where Clojure
defines a cross-type equivalence:

```clojure
(= [1 2 3] [1 2 3])            ;; => true
(= [1 2 3] '(1 2 3))           ;; => true   — vectors and lists are seq-equal
(= {:a 1 :b 2} {:b 2 :a 1})    ;; => true   — maps are unordered
(= #{1 2 3} #{3 2 1})          ;; => true   — sets are unordered

(= [1 2 3] '(1 2 3) (seq [1 2 3]))
                                ;; => true
```

`==` is the *numeric* equality across number types, with no cross-type
equivalence for collections:

```clojure
(== 1 1.0 1/1)                 ;; => true
(== [1] [1])                    ;; raises — `==` is for numbers
```

`identical?` is pointer equality. You rarely want it; the cases that
do are sentinel comparisons.

## 4.9 Hashing

Maps and sets use a structural hash that is *consistent with `=`*: any
two values that `=` produce the same hash. The protoCore hash is the
substrate's. You can call `hash` directly:

```clojure
(hash :a)                      ;; => some long
(hash {:a 1 :b 2})             ;; => some long
(= (hash [1 2]) (hash '(1 2))) ;; => true (because they are = and hash agrees)
```

In practice, this matters if you put your own types (records, when v0.2
adds them) into sets and as map keys. Implement structural hash properly
and equality / set membership just work.

## 4.10 The substrate underneath

Brief, because it matters when you are debugging or profiling:

- **List** → protoCore `ProtoList` in linked-list mode (head pointer +
  tail pointer to the next cell).
- **Vector** → protoCore `ProtoList` in AVL-indexed mode (balanced tree
  with array leaves).
- **Map** → protoCore `ProtoSparseList` (hash-array-mapped trie of
  bucket nodes).
- **Set** → protoCore `ProtoSparseList`, keyed by element with `true`
  values.
- **String** → protoCore `ProtoString` (rope-backed UTF-8, structurally
  shared on concatenation).

The same primitives serve protoPython's `list` / `dict` / `set` and
protoJS's `Array` / `Map` / `Set` and protoST's `Array` / `Dictionary`.
A vector built in Clojure and handed to a Python function through the
UMD bridge is *the same object* — the Python side sees it as a list
through its language adapter, but the underlying `ProtoList` is shared.

Conversion is only needed when the target language's collection API
demands a *different* shape — e.g., NumPy wants a `numpy.ndarray`, not
just a sequence. Chapter 9 covers when to reach for `clj->py` /
`py->clj` and friends.

## 4.11 A short worked example

A vocabulary frequency counter. Stays in vectors and maps; threads with
`->>`; closes with a sort:

```clojure
(defn word-freq [text]
  (->> text
       clojure.string/lower-case
       (re-seq #"\w+")            ;; seq of words
       frequencies                  ;; map word→count
       (sort-by (comp - val))       ;; seq of [word count] desc by count
       vec))                        ;; back to vector

(word-freq "the cat sat on the mat the cat sat")
;; => [["the" 3] ["cat" 2] ["sat" 2] ["on" 1] ["mat" 1]]
```

Every collection operation returned a new value. Nothing in the input
was mutated. The result fits in your head because each `->>` step is
one transformation. This is the shape of most idiomatic Clojure code.

Next: [Chapter 5 — Functions and closures](05-functions-and-closures.md).
