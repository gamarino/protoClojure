# 9. Cross-Runtime Interop

This is the chapter that earns the project its existence. Everything
covered so far — persistent data, REPL-driven, functional core — exists
in other Clojure dialects. The cross-runtime UMD interop does not.

The mechanics fit on a half-page; the implications fill a chapter.

## 9.1 The half-page mechanics

In a `(:require [...])` clause, a namespace name with a *prefix*
selects the language provider:

```clojure
(ns my.app
  (:require [clojure.string :as str]      ;; unprefixed → Clojure path
            [py/numpy :as np]              ;; py/   → protoPython
            [js/lodash :as _]              ;; js/   → protoJS
            [pst/Counter :as ctr]))         ;; pst/  → protoST
```

The resolver:

1. Sees `py/numpy`, splits on the first `/`, picks the **protoPython
   provider**, asks it for `numpy`.
2. The Python provider runs its normal import machinery and returns a
   module object — a `ProtoObject` with attributes for each member of
   `numpy`.
3. The Clojure compiler aliases `np` to that module object in the
   current namespace.

Calls work as you expect:

```clojure
(np/array [1 2 3])           ;; calls numpy.array on a Clojure vector
(np/sum some-array)          ;; calls numpy.sum
(np/.shape some-array)       ;; dotted-form: attribute access
```

The dotted form (`(np/.shape arr)` or equivalently `(.shape arr)`
when receiving as the first arg) reads attributes that are not
callables; useful for properties on Python objects.

That is the surface. The rest of the chapter is about *when* the
abstraction is honest and *when* you reach for explicit conversion.

## 9.2 Primitives pass through, untouched

Numbers, strings, booleans, and nil are *the same object* across all
four runtimes:

```clojure
(:require [py/math :as m])

(def py-pi (m/pi))           ;; Python's math.pi → 3.141592653589793

(* 2 py-pi)                  ;; ordinary Clojure arithmetic
;; => 6.283185307179586

(str "pi is " py-pi)         ;; ordinary Clojure string concat
;; => "pi is 3.141592653589793"
```

There is no marshalling step. The Python value `3.141592653589793` and
the Clojure value `3.141592653589793` are the **same `Float` cell**
inside protoCore, with the same prototype chain. They were the same
object before they crossed the language boundary because the boundary
is a label, not a copy.

This is the substrate paying off. A pandas DataFrame method that
returns a number returns a number protoClojure code can `+` and
`println` with no thought.

## 9.3 Functions pass through too

A Python function is a callable protoCore object. So is a Clojure
function. Both respond to the same invoke protocol. So:

```clojure
(:require [py/functools :as ft])

(def py-reduce ft/reduce)

(py-reduce + (range 100))    ;; calls Python's reduce, returns a Clojure integer
;; => 4950
```

You can pass Clojure functions to Python functions that expect
callables:

```clojure
(np/.apply_along_axis (fn [row] (reduce + row))   ;; Clojure function
                       1                            ;; axis 1
                       some-matrix)
```

NumPy calls the Clojure function row by row, each call returns a
number, NumPy assembles them into the result. No FFI, no marshalling,
no glue code.

The same applies to JavaScript functions and protoST blocks. A
protoST `Block` is also a callable; a Clojure higher-order function
that takes a callable will take a protoST block just as well.

## 9.4 Collections need explicit conversion

This is the leaky part of the abstraction, and it is leaky **on
purpose**. A Python `list` and a Clojure `vector` are different
protoCore objects with different prototypes and different invariants.
Auto-converting at the boundary would either:

- Quietly lose Clojure's persistence (a Python list is mutable),
  silently breaking code that assumed the Clojure vector was
  unchanged.
- Walk every element on every cross-boundary call, paying `O(n)` you
  did not see coming.

Instead, conversion is **named and explicit**:

```clojure
(clj->py {:a 1 :b 2})        ;; Clojure map → Python dict
(py->clj some-dict)          ;; Python dict → Clojure map
(clj->js [1 2 3])            ;; Clojure vector → JavaScript array
(js->clj some-array)         ;; JavaScript array → Clojure vector
(clj->pst {:k :v})           ;; Clojure map → protoST Dictionary
(pst->clj some-st-dict)      ;; protoST Dictionary → Clojure map
```

The rules:

- Vector → list / Array / OrderedCollection in the target language.
- Map → dict / Object / Dictionary.
- Set → set / Set / Set.
- Keyword → string (no native equivalent in Python or JS).
- Symbol → string.

Conversion is **shallow by default**. A nested map of vectors stays a
Clojure map at the top level after `clj->py` — only the outer level
becomes a Python dict. For deep conversion:

```clojure
(clj->py {:a [1 2] :b {:c 3}} :deep true)
;; => {"a": [1, 2], "b": {"c": 3}}
```

This is the explicit price. Most cross-language code reaches for
`*->clj` and `clj->*` exactly twice — once at the boundary going in,
once at the boundary coming out — and is otherwise written in one
language's idiom.

## 9.5 Worked example — NumPy matrix operations from Clojure

A statistics function over a matrix, using NumPy for the heavy
arithmetic and Clojure for the orchestration:

```clojure
(ns demo.matrix-stats
  (:require [py/numpy :as np]))

(defn col-stats [rows-vec]
  ;; rows-vec is a Clojure vector of vectors.
  ;; NumPy wants a real ndarray; we convert once and stay in NumPy
  ;; for the column-wise reductions.
  (let [arr   (np/array (clj->py rows-vec :deep true))
        means (np/mean arr :axis 0)
        stds  (np/std arr :axis 0)]
    {:means (py->clj means)
     :stds  (py->clj stds)}))

(col-stats [[1.0 2.0 3.0]
            [4.0 5.0 6.0]
            [7.0 8.0 9.0]])
;; => {:means [4.0 5.0 6.0], :stds [2.449... 2.449... 2.449...]}
```

The work itself runs in NumPy's C-level loops. The orchestration
(name what we are doing, return as a Clojure map) is in Clojure. The
boundary is one `clj->py` and two `py->clj` calls — explicit,
visible, easy to spot in code review.

## 9.6 Named arguments to foreign functions

Python and JavaScript both have named-argument calling conventions
that Clojure does not. The bridge:

```clojure
(np/array [1 2 3] {:dtype "float32"})        ;; trailing map → keyword args
;; ≡ Python: numpy.array([1, 2, 3], dtype="float32")

(d3/.attr selection {:fill "red" :stroke "black"})
;; ≡ JS: selection.attr({fill: "red", stroke: "black"})
```

The convention: a trailing Clojure map is unpacked as keyword arguments
when the foreign function expects them. Foreign functions whose
metadata declares no keyword arguments treat the map as a positional
arg (the map itself).

`(meta np/array)` shows you the function's signature so you know which
arguments are positional and which are by name. The same `:arglists`
metadata convention as Clojure's own functions; for foreign functions
the bridge populates it from the language's introspection
(`inspect.signature` for Python, the function's `.length` plus
docstring scrape for JS).

## 9.7 Errors at the boundary

A Python exception raised inside a NumPy call becomes a Clojure
exception of class `ForeignException`. The original exception is
accessible through `ex-data`:

```clojure
(try
  (np/array nil)
  (catch ForeignException e
    (println "Python error:" (:python-class (ex-data e)))
    (println "Message:"      (ex-message e))
    (println "Trace:"         (:traceback (ex-data e)))))
```

JS errors and protoST exceptions follow the same pattern. The
common-ancestor protoCore `Exception` class means a single
`(catch Exception e ...)` matches anything from anywhere.

## 9.8 Going the other direction — Clojure as a UMD provider

Symmetry. Every protoClojure namespace is a UMD module that any
other runtime can consume.

From protoPython:

```python
import clj.demo.greeting as greeter
print(greeter.greet("alice"))   # → "hello, alice"
```

From protoJS:

```javascript
const greeter = require('clj/demo.greeting');
console.log(greeter.greet("alice"));
```

From protoST:

```smalltalk
greeter := Import from: 'clj/demo.greeting'.
greeter greet: 'alice'.
```

The Python or JS side sees Clojure functions as native callables,
Clojure maps as `Mapping`-protocol objects (read-only — Clojure's
maps are persistent and refuse mutation), Clojure vectors as
`Sequence`-protocol objects.

This is the part of the design that makes the ecosystem useful: any
language can produce, any language can consume.

## 9.9 The whole tri-runtime example

The script from Chapter 1, now you can read every line:

```clojure
(ns demo.tri-runtime
  (:require [py/pandas :as pd]
            [js/d3 :as d3]
            [clojure.string :as str]))

(defn analyse [csv-path]
  (let [df    (pd/read_csv csv-path)         ;; pandas reads the CSV
        cols  (py->clj (pd/.columns df))]    ;; Python list → Clojure vec
    (->> cols
         (filter #(str/starts-with? % "metric_"))
         (map (fn [col]
                (let [series (pd/.loc df :all col)
                      values (py->clj series)]
                  {:col col
                   :sum (reduce + values)
                   :avg (/ (reduce + values) (count values))}))))))

(defn -main [csv-path out-path]
  (let [results (analyse csv-path)
        data    (clj->js results :deep true)] ;; Clojure → JS for the renderer
    (d3/renderBarChart data out-path)
    (println "Wrote chart to" out-path)))
```

What is in each step:

- `(pd/read_csv csv-path)` — pandas, Python, returns a pandas
  DataFrame as a protoCore object.
- `(pd/.columns df)` — Python attribute access; returns a pandas
  Index object that responds to `len`, `[i]`, iteration.
- `(py->clj ...)` — explicit conversion at the boundary, once. From
  here on, the column names are an idiomatic Clojure vector of
  strings.
- `(->>` ... `filter` ... `map ...)` — pure Clojure, threading
  through the columns we care about. Each closure pulls a column from
  the DataFrame (`pd/.loc`) and converts it once to a Clojure seq for
  the `reduce`.
- The map at the end of the `(map ...)` step is a Clojure map. The
  whole `results` is a seq of Clojure maps.
- `(clj->js results :deep true)` — explicit conversion at the
  boundary again, deeply. From here on, the data structure is a JS
  array of objects that D3 understands natively.
- `(d3/renderBarChart data out-path)` — JS, draws the chart to a file.

Three runtimes. One language wrapper. The data structures live in
*one* memory space throughout — only the language-side wrappers
change. That is the substrate doing its job.

## 9.10 A short checklist for cross-runtime code

- [ ] Did I `:require` the foreign module with the right prefix?
- [ ] Am I converting at the boundary, not in the middle of the
  pipeline?
- [ ] Did I convert *once* each direction, not on every call?
- [ ] If I am holding the foreign value as state, do I want it
  converted, or do I want to keep operating on it through the
  foreign API?
- [ ] Is the function I am calling pure on its side, or does it have
  side effects (state in NumPy, DOM in D3)? The Clojure side does
  not protect you from foreign mutation.

Next: [Chapter 10 — The REPL](10-repl.md).
