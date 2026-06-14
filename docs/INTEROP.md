# protoClojure — Module System and Cross-Runtime Interop

> **The unique value proposition.** Clojure-JVM has Java. protoClojure
> has Python, JavaScript, and Smalltalk, all through one resolver. This
> document explains how that works and where the seams are.

> **Implementation status (session 12).** Everything in this document
> is design — the providers (`py/`, `js/`, `pst/`, `clj/`) are scoped
> for Phase 5 of `ROADMAP.md` (sessions 22-25). The positional half of
> the protoCore call convention works today (a `defn`-defined function
> is reachable from protoST and protoPython with positional args); the
> named-arg half is the priority of session 13. Until the providers
> land, `(:require [py/numpy])` raises a clear "UMD provider not yet
> registered" error.

---

## 1. The hybrid module system at a glance

A protoClojure file declares its dependencies with a standard `ns`
form. The Clojure syntax is unchanged:

```clojure
(ns my.app
  (:require [clojure.string :as str]
            [my.util :refer [helper]]
            [py/numpy :as np]
            [js/lodash :as _]))
```

Underneath, every `(:require [X :as A])` resolves through the **UMD
registry** — the same universal-module-discovery chain that protoCore
provides to protoPython, protoJS, and protoST. The Clojure programmer
sees no extra system; the protoCore programmer sees one more language
consumer.

The decisive rule: **the symbol's namespace prefix selects the
language provider.**

| Prefix         | Provider                              | Example                               |
| -------------- | ------------------------------------- | ------------------------------------- |
| (none)         | Clojure path resolver                 | `[clojure.string :as str]`            |
| `py/`          | protoPython UMD provider              | `[py/numpy :as np]`                   |
| `js/`          | protoJS UMD provider                  | `[js/lodash :as _]`                   |
| `pst/`         | protoST UMD provider                  | `[pst/Counter :as ctr]`               |
| `clj/`         | Clojure path (explicit)               | `[clj/clojure.string :as str]`        |

The unprefixed form is sugar for `clj/` — the runtime checks if the
namespace string contains a `/` separator at the first segment level
and, if not, treats the whole thing as a Clojure namespace.

## 2. The resolver chain in detail

When the runtime sees `(:require [X :as A])` at module load time:

1. **Lex the namespace**: split `X` on the first `/` (if any). The left
   part is the *provider tag* (or empty for default); the right part is
   the *provider-local name*.

2. **Look up the provider** in the UMD provider registry (a global per-
   `ProtoSpace` map of `tag → UMDProvider`):
   - empty / `clj/` → Clojure source provider (file-system, walks
     `CLOJURE_PATH`).
   - `py/` → protoPython provider (delegates to protoPython's import
     machinery if protoPython is loaded; otherwise raises a clear
     "protoPython runtime not available" error).
   - `js/` → protoJS provider.
   - `pst/` → protoST provider.
   - User-registered providers extend this table.

3. **Provider resolves the local name** to a *module object*. For the
   Clojure provider, this means reading the `.clj` source, compiling
   it, and returning the namespace object. For `py/`, this means
   calling protoPython's import machinery; the result is a protoPython
   module object (a `ProtoObject` with attributes for each module
   member). For `js/`, equivalent for protoJS. For `pst/`, equivalent
   for protoST.

4. **Cache** the resolved module under its full namespaced name in the
   UMD registry's module cache. Subsequent `require`s on the same name
   from any runtime return the cached object.

5. **Bind the alias** in the current Clojure namespace: `:as A` adds
   the alias `A` → module-object to the namespace; `:refer [x y]`
   binds `x` and `y` directly to the corresponding attributes of the
   module-object.

The whole resolver is the same code path that protoPython uses for
`import numpy as np`, that protoJS uses for `require('lodash')`, and
that protoST uses for `Import from: 'X'`. **There is one module
system, four surface syntaxes.**

## 3. Calling foreign module members

Once `:as A` is bound, `A/x` is sugar for "attribute `x` of the module
object stored under alias `A`". The Clojure compiler emits the same
attribute-read bytecode it uses for any var reference; the dispatch is
the universal protoCore `getAttribute` chain walk.

```clojure
(:require [py/math :as m])

(m/sqrt 2.0)
;; ≡ ((. m sqrt) 2.0)
;; ≡ (.sqrt m 2.0)  — Clojure-JVM dotted form is also accepted
```

Function application uses the same dispatch protoClojure uses for
its own functions: protoCore checks the callable's prototype for an
invoke method and dispatches. A protoPython function answers to the
invoke protocol because the protoPython runtime installs it on every
Python function object; same for JS, same for ST. No marshalling layer.

### 3.1 The protoCore call convention

protoCore methods take `(name, positional_args, named_args)`. Most of
what we call through the interop boundary expects to be called this
way — protoPython functions and protoJS functions both translate cleanly.
The Clojure side packs the call as positional arguments by default,
with named arguments expressible through a final keyword-map arg:

```clojure
(np/array [[1 2] [3 4]])
(np/sum arr {:axis 1})
;; ≡ Python: numpy.sum(arr, axis=1)
```

A leading map of named arguments would be ambiguous; we only treat the
trailing map as the keyword-args block, and only when the foreign
function expects named arguments. (The function's metadata exposes
this; see §6.)

## 4. Type coercion at the boundary

Three categories of value cross the interop boundary:

### 4.1 Primitives — pass through

`SmallInteger`, `LargeInteger`, `Float`, `Boolean`, `nil`, and
`ProtoString` are the *same* `ProtoObject` regardless of the runtime
that created them. A `42` from Python is a `42` in Clojure is a `42`
in JavaScript — bit-identical, same allocation, no conversion.

```clojure
(def py-pi (py/math.pi))
(* 2 py-pi)                    ;; ordinary Clojure arithmetic on a Python value
```

### 4.2 Functions — pass through

A Python function or a JavaScript function is callable from Clojure
with no wrapping. The function's invoke protocol is on its prototype;
protoClojure calls it like any other function.

```clojure
(:require [py/functools :refer [reduce]])

(def py-reduce reduce)
(py-reduce + (range 10))       ;; Python's reduce, called from Clojure
;; => 45
```

(In this specific case JVM `clojure.core/reduce` shadows the imported
one, so the user would alias it. The illustration stands.)

### 4.3 Collections — explicit conversion required

This is the one place where the abstraction is leaky, and it is leaky
**by design**.

A Python `list` and a Clojure `vector` are both sequenceable, both
indexable, both serialisable — but they are *different protoCore
objects* with different prototypes, different invariants
(immutability), and different access protocols. Auto-converting at
the boundary would either be:

- **Lossy in one direction.** A Clojure vector copied to a Python list
  loses its persistence — subsequent Clojure code holding a reference
  to the vector still sees the original, but Python sees a mutable copy.
  A surprising abstraction.
- **Surprisingly expensive.** Every cross-runtime call would walk every
  element of every collection arg. The whole point of "primitives are
  the same object" is to *avoid* that cost.

Instead, conversion is **explicit and named**:

| From → To       | Function   | Behaviour                                                    |
| --------------- | ---------- | ------------------------------------------------------------ |
| Clojure → Py    | `clj->py`  | Returns a fresh Python collection of converted elements.     |
| Py → Clojure    | `py->clj`  | Returns a fresh Clojure collection of converted elements.    |
| Clojure → JS    | `clj->js`  | Returns a fresh JS object/array of converted elements.       |
| JS → Clojure    | `js->clj`  | Returns a fresh Clojure collection of converted elements.    |
| Clojure → ST    | `clj->pst` | Returns a fresh protoST collection of converted elements.    |
| ST → Clojure    | `pst->clj` | Returns a fresh Clojure collection of converted elements.    |

Conversion rules (illustrated for `clj->py`; mirror for the others):

- Clojure vector → Python list
- Clojure list → Python list
- Clojure map → Python dict
- Clojure set → Python set
- Clojure keyword → Python string `"foo"` (no native equivalent)
- Clojure symbol → Python string
- Primitives unchanged

Conversion is **shallow by default**; for a nested structure, pass
`:deep true`:

```clojure
(clj->py {:a [1 2] :b {:c 3}} :deep true)
;; => {"a": [1, 2], "b": {"c": 3}}
```

## 5. Calling Clojure from another runtime

protoClojure is symmetric: every protoClojure namespace is a UMD
module that any other runtime can consume. A protoPython script can:

```python
import clj.my_app as app
result = app.greet("alice")
```

The protoPython UMD provider has a Clojure backend hook that the
protoClojure runtime registers at startup. The mapping is the inverse
of §4: Clojure functions appear to Python as Python callables, Clojure
maps appear as `Mapping`-protocol objects (read-only), Clojure vectors
appear as `Sequence`-protocol objects.

The same applies to protoJS (`require('clj/my-app')`) and protoST
(`Import from: 'clj/my-app'`).

This direction has one constraint: **Clojure namespace files are
identified by `.clj` extension**, and the cross-runtime importer maps
the consumer's module name to a `.clj` filename. So `clj.my_app` in
Python maps to `my_app.clj` (underscore stays — the Python provider
does not translate underscores to dashes, by convention). `clj/my-app`
in JS / ST maps to `my-app.clj`. Document, do not magic.

## 6. Function metadata across the boundary

A Python function exposes `__doc__`, `__annotations__`, `__name__`.
A JavaScript function exposes `name`, `length`. A Clojure function
exposes its var's `:doc`, `:arglists`, etc.

protoClojure unifies these under a single accessor:

```clojure
(meta np/sum)
;; => {:name "sum"
;;     :doc  "Sum of array elements over a given axis."
;;     :arglists ([a axis dtype out keepdims initial where])
;;     :provider :py
;;     :module "numpy"}
```

The `:arglists` are extracted from the foreign function's
introspection where available; for Python this is `inspect.signature`,
for JS this is the argument-count + a NaN-arg-name signature, for ST
this is the method's call-form signature where available.

`:arglists` informs the optional named-args call form (§3.1).

## 7. Errors at the boundary

A Python exception raised inside a Clojure call becomes a Clojure
exception; the original Python exception is wrapped in
`ForeignException` with its message and traceback accessible via
`ex-data`:

```clojure
(try
  (np/array nil)
  (catch ForeignException e
    (println "Python error:" (:python-class (ex-data e)))
    (println "Message:"      (ex-message e))))
```

The same applies to JS errors and protoST exceptions. The protoCore
exception hierarchy already provides the catch-all `Exception` /
`Error` that all four runtimes hang their language-specific subclasses
off, so a `(catch Exception e ...)` matches anything.

## 8. The Clojure path resolver

Unprefixed `(:require [foo.bar :as fb])` walks the **CLOJURE_PATH**
environment variable (a colon-separated list of directories), plus
the current source file's directory, looking for `foo/bar.clj`. The
first match wins.

If `foo/bar` resolves to a directory with a `_loader.clj` (TBD)
inside, that loader is used; useful for multi-file packages.

Mutations:
- `(:as A)` binds `foo.bar` under alias `A` in the current namespace.
- `(:refer [x y])` binds `foo.bar/x` and `foo.bar/y` directly.
- `(:refer :all)` binds everything publicly exported by `foo.bar`.

The same `ns` form may freely mix unprefixed and prefixed requires —
they're all UMD resolution under the hood.

## 9. Performance characteristics

- **Cold load**: depends entirely on the foreign provider. Loading
  NumPy through `py/` triggers protoPython's NumPy import, which is
  slow (~hundreds of ms). Loading `clojure.string` is a Clojure file
  parse + compile, ~ms.
- **Cached load**: returns the cached module object instantly.
- **Call overhead**: a foreign-function call costs one extra
  attribute lookup compared to a same-runtime call. In the steady
  state this is invisible.
- **Conversion cost**: shallow `clj->py` of an N-element vector is
  `O(N)` plus the allocation of the target. Deep is `O(total nodes)`.
  This is the cost the user is paying for to keep the abstraction
  honest; the runtime makes it visible by requiring the explicit call.

## 10. Worked example — the demo of demos

A 30-line script combining all four runtimes:

```clojure
(ns demo.tri-runtime
  (:require [py/pandas :as pd]
            [py/io :as pio]
            [js/d3 :as d3]
            [pst/Counter :as ctr]
            [clojure.string :as str]))

(defn analyse [csv-path]
  ;; Load CSV via pandas (Python).
  (let [df    (pd/read_csv csv-path)
        cols  (py->clj (pd/.columns df))]
    ;; Process functionally in Clojure.
    (->> cols
         (filter #(str/starts-with? % "metric_"))
         (map (fn [col]
                (let [series (pd/.loc df :all col)
                      values (py->clj series)]
                  ;; Push each metric into a protoST counter for
                  ;; side-by-side comparison with the protoST tutorial.
                  (ctr/init :name col :total (reduce + values))
                  {:col col
                   :sum (reduce + values)
                   :avg (/ (reduce + values) (count values))})))
         doall)))

(defn render [results out-path]
  ;; Render an SVG bar chart via D3 (JavaScript).
  (let [data (clj->js results :deep true)]
    (d3/renderBarChart data out-path)))

(defn -main [csv-path out-path]
  (let [results (analyse csv-path)]
    (render results out-path)
    (println "Wrote chart to" out-path)))
```

This is the demo that goes on the README, in conference talks, and on
the blog post that announces v0.1. Until the runtime ships, the script
above is also the *executable spec* — every primitive it uses has to
work for the launch to be honest.
