# 12. A Worked Example

This is the chapter where we tie the tutorial together. We will take
the tri-runtime script from Chapter 1 — pandas reads a CSV, Clojure
processes it, D3 renders a chart — and walk through it one line at a
time. Every concept the previous eleven chapters introduced will
have at least one job to do here.

By the end you should be able to read the script and, more
importantly, write a variant: load a different data source, run a
different analysis, render a different output. The whole point of
the substrate is that the *shape* of cross-runtime work is
straightforward; only the domain is specific.

## 12.1 The full script, again

```clojure
(ns demo.tri-runtime
  (:require [py/pandas :as pd]
            [js/d3 :as d3]
            [clojure.string :as str]))

(defn analyse [csv-path]
  (let [df    (pd/read_csv csv-path)
        cols  (py->clj (pd/.columns df))]
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
        data    (clj->js results :deep true)]
    (d3/renderBarChart data out-path)
    (println "Wrote chart to" out-path)))
```

We will go top to bottom.

## 12.2 The namespace declaration

```clojure
(ns demo.tri-runtime
  (:require [py/pandas :as pd]
            [js/d3 :as d3]
            [clojure.string :as str]))
```

**Chapter 8** taught us that `(ns name ...)` switches to a new
namespace and processes its requires. **Chapter 9** explained the
prefixes: `py/` and `js/` route through the protoPython and protoJS
UMD providers; the unprefixed `clojure.string` goes through the
Clojure path resolver.

Three things happen at namespace load time:

1. The Python provider imports `pandas`. If protoPython is not
   loaded, you get a clear error pointing at it. If pandas is not
   installed in the protoPython environment, you get the Python
   `ModuleNotFoundError` propagated. The result, on success, is the
   pandas module object bound under the alias `pd`.
2. The JS provider does the equivalent for `d3`. If D3 is not in the
   protoJS module path, error.
3. `clojure.string` is a file on the Clojure path; it is loaded,
   bound under `str`.

After this form, `pd`, `d3`, and `str` are aliases in the current
namespace's resolver table. `pd/read_csv` resolves to the
`read_csv` attribute on the pandas module object. The dispatch is
the same `getAttribute` chain walk every other namespace alias
uses.

## 12.3 The `analyse` function — shape and threading

```clojure
(defn analyse [csv-path]
  (let [df    (pd/read_csv csv-path)
        cols  (py->clj (pd/.columns df))]
    (->> cols
         ...)))
```

**Chapter 5** taught us `defn` and `let`. **Chapter 8** taught us
that `:require ... :as pd` makes `pd/x` mean "the `x` attribute of
the pandas module". `pd/read_csv` is a Python function we are
calling with one argument; the result — a pandas `DataFrame` — is a
protoCore object bound to `df`.

`(pd/.columns df)` is the dotted-form attribute access from
**Chapter 9** §9.1: it reads the `columns` attribute of `df`. That
attribute is a pandas `Index` object — sequence-like, holds
string column names.

`(py->clj (pd/.columns df))` — **Chapter 9 §9.4** — converts the
foreign sequence to a Clojure vector. We do this once, at the
boundary going in, exactly as the chapter recommended. From here on,
`cols` is an idiomatic Clojure vector of strings.

The `->>` macro — **Chapter 2 §2.8** — threads `cols` through the
rest as the *last* argument of each step. Reads top-to-bottom.

## 12.4 The first thread step — `filter`

```clojure
(filter #(str/starts-with? % "metric_"))
```

**Chapter 2 §2.6** introduced `#(...)` as the anonymous-function
shorthand; `%` is the first argument. So this is the filter
predicate "the column name starts with `metric_`".

**Chapter 4 §4.7** noted that `filter` returns a lazy seq. We pay
attention to that briefly: nothing has *actually been filtered* yet.
The lazy seq will produce elements on demand. Threading forward to
`map` and then `reduce` does not change that — the chain is set up,
the work happens when the final consumer asks for it.

`str/starts-with?` is from `clojure.string`. The qualifier `str/`
comes from `:as str` in the `ns` form.

## 12.5 The mapping step — per-column statistics

```clojure
(map (fn [col]
       (let [series (pd/.loc df :all col)
             values (py->clj series)]
         {:col col
          :sum (reduce + values)
          :avg (/ (reduce + values) (count values))})))
```

This is the part that does real work. `map` takes a function and our
filtered seq of column names; it returns a lazy seq of "result
maps", one per column.

The inner function takes one column name and computes a map for it.
Let us walk it line by line.

**`(pd/.loc df :all col)`** — pandas's `.loc` accessor. `(df.loc[:,
col])` in Python. The first arg `:all` is the protoClojure
equivalent of the Python slice `:` (all rows). The second is the
column name we are extracting.

> **A small bridge.** Python's slice syntax (`df.loc[:, col]`) is
> not Clojure-expressible. The protoPython UMD bridge maps the
> Clojure keyword `:all` to the Python `slice(None, None)` object,
> giving us a Clojure-side spelling for "all rows". This convention
> is documented in [`INTEROP.md`](../INTEROP.md) §3.

The result, `series`, is a pandas `Series` — a one-dimensional
labelled array. Foreign.

**`(py->clj series)`** — second boundary conversion, again at the
boundary. From here, `values` is a Clojure vector of numbers.

**`(reduce + values)`** — sum. **Chapter 5 §5.8** introduced
`reduce`. **Chapter 4** taught us that `+` is a function like any
other; you can pass it as an argument.

**`(count values)`** — vector length, O(log n).

**`(/ (reduce + values) (count values))`** — average. This is a
small inefficiency: we are computing the sum twice. A real
implementation would `let`-bind the sum once. The point of the
example is to show the *shape*, not to be optimal.

The returned map `{:col col :sum (...) :avg (...)}` is the
per-column result. The whole chain produces a lazy seq of these
maps.

## 12.6 The thread completes

The `->>` chain at the end of `analyse` returns a lazy seq of
column-stat maps. The function returns. *Nothing has been
forced yet* — the seq is unrealised.

This is on purpose. We will hand the seq to the next step
(`-main`'s `clj->js`) and let *that* force realisation. The
laziness threads through naturally; one operation can stop early
if it needs to.

```clojure
;; Conceptually:
(analyse "data.csv")
;; => (lazy seq of {:col ..., :sum ..., :avg ...})
```

## 12.7 The `-main` entry point

```clojure
(defn -main [csv-path out-path]
  (let [results (analyse csv-path)
        data    (clj->js results :deep true)]
    (d3/renderBarChart data out-path)
    (println "Wrote chart to" out-path)))
```

`-main` is the convention for an executable entry point — the script
runner calls `(-main & args)` after loading the namespace. Two
arguments here: the input CSV and the output chart path.

**`(analyse csv-path)`** — produces the unrealised lazy seq.

**`(clj->js results :deep true)`** — **Chapter 9 §9.4**. Boundary
conversion to JavaScript, deep because D3 expects native JS arrays
and objects all the way down. The deep flag causes:

- The outer seq → JS array.
- Each Clojure map → JS plain object (with the Clojure keywords
  becoming JS strings).
- The string values stay strings; the numbers stay numbers (they
  are the same protoCore primitives across runtimes).

`clj->js :deep true` is the step that *forces realisation* of the
lazy seq — the walker descends every element to convert it, so every
element is computed. The pandas accesses and the reductions happen
here.

**`(d3/renderBarChart data out-path)`** — call into D3. The
`renderBarChart` function is, in this hypothetical setup, a small
D3 wrapper we have loaded; it does whatever bar-chart rendering D3
does and writes the SVG to `out-path`.

**`(println "Wrote chart to" out-path)`** — print confirmation. The
script is done.

## 12.8 What the substrate is doing during all this

A summary of the cross-runtime memory picture during execution:

1. `pd/read_csv` reads the CSV into pandas's columnar storage. That
   memory is in protoCore allocations, managed by protoCore's
   garbage collector.
2. The pandas `DataFrame` object lives in protoCore as a single
   wrapper carrying pointers to the columnar arrays.
3. `(py->clj (pd/.columns df))` walks the Python `Index` object's
   sequence protocol and produces a Clojure vector of strings. The
   strings themselves are the same `ProtoString` objects pandas was
   carrying — no character copies. Only the outer container shape
   changes from "Python Index" to "Clojure vector".
4. The lazy seq of result maps holds back-references to `df` and to
   `cols`. Both are alive in protoCore as long as the seq is alive.
5. `clj->js :deep true` walks the seq, converting each Clojure
   map to a JS plain object. The numbers in the maps (sums, averages)
   pass through as the same primitive values — D3 reads them as
   plain JS numbers.
6. D3 does its rendering; the SVG goes to disk. After `-main`
   returns, nothing references `df` or any intermediate; the GC
   reclaims it on its next cycle.

The whole run is one protoCore process, one GC, three runtime
languages, one data space. *That is the value proposition the rest
of the tutorial has been building toward.*

## 12.9 Variations you can write now

If you can read the above, you can write:

- **Different statistics**: change the map function inside `analyse`
  to compute median, stddev, quantiles. NumPy is available — you
  can `(:require [py/numpy :as np])` and use `np/median`,
  `np/std`.
- **Different input**: replace `pd/read_csv` with `(slurp some-url)`
  and a manual parse, or with `pd/read_json` for JSON, or with a
  protoJS `fetch` call.
- **Different output**: replace `d3/renderBarChart` with a Python
  matplotlib call, or with a protoClojure-native SVG writer, or
  with a CSV report (just `spit` it).
- **Streaming version**: instead of `analyse` returning a realised
  seq, have it return a transducer-shaped pipeline (v0.2 — wait
  for transducers) and pipe to a sink.

The pattern is the same: read at the boundary with `*->clj`, work in
Clojure, write at the boundary with `clj->*`. Names, types, and
shapes change; the architecture does not.

## 12.10 What to read next

This is the last tutorial chapter. From here, the recommended
sequence:

- **The full language reference**: [`LANGUAGE.md`](../LANGUAGE.md).
  Everything not covered in detail in the tutorial.
- **The interop deep dive**: [`INTEROP.md`](../INTEROP.md). The
  rules for resolution chains, named arguments, error
  propagation, and metadata bridging.
- **The status of v0.1**: [`STATUS.md`](../STATUS.md). What is
  implemented today versus what is scheduled.
- **The design rationale**: [`DESIGN.md`](../DESIGN.md). Why
  protoClojure is the shape it is.

If you find a place where the documentation and the runtime disagree,
the runtime is wrong — file an issue. If you find a place where the
runtime and JVM Clojure disagree without explanation, check
`STATUS.md` first; if it is not a documented deviation, it is also
a bug.

Welcome to protoClojure.
