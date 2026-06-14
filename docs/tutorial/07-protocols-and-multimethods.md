# 7. Protocols and Multimethods

Clojure does polymorphism without classes. Two mechanisms:

- **Protocols**: a set of named functions whose implementation can be
  *extended* to any type. Open over types, closed over methods. Fast
  dispatch — one attribute lookup.
- **Multimethods**: a single function whose implementation is
  selected by an arbitrary *dispatch function*. Open over both types
  and dispatch logic. Slower dispatch — table lookup with hierarchy.

Both let you add behaviour to existing types without modifying them.
That is the whole point. In Java / Python class-based code, "adding a
method to a class you don't own" is a recurring pain point; in
Clojure, it is a one-liner.

This chapter teaches both. On protoCore the implementations are
particularly natural — *protocols are just attributes on prototypes*,
which is what protoCore does for everything.

## 7.1 Protocols

A protocol declares a set of method names with arglists:

```clojure
(defprotocol Renderable
  "A thing that can be drawn to a screen."
  (render [this surface]    "Draw this onto surface.")
  (bounds [this]            "Return the bounding rectangle.")
  (hit?   [this x y]        "True if (x, y) is inside this."))
```

`defprotocol` declares the protocol `Renderable` and three functions
`render`, `bounds`, `hit?`. None of them have implementations yet.
Calling `(render thing surface)` looks up `render` on `thing`'s
prototype chain — if there is no `render` registered for `thing`'s
type, you get `No implementation of method: :render of protocol:
Renderable found for class: …`.

Implementations come from `extend-type`:

```clojure
(defrecord-or-map Square [side x y])    ;; v0.1: just a plain map

(extend-type Square
  Renderable
  (render [this surface]   (draw-square surface (:x this) (:y this) (:side this)))
  (bounds [this]            {:x (:x this) :y (:y this)
                              :w (:side this) :h (:side this)})
  (hit?   [this x y]        (let [s (:side this)]
                              (and (<= (:x this) x (+ (:x this) s))
                                   (<= (:y this) y (+ (:y this) s))))))

(extend-type Circle
  Renderable
  (render [this surface]   (draw-circle surface (:x this) (:y this) (:r this)))
  (bounds [this]            {:x (- (:x this) (:r this))
                              :y (- (:y this) (:r this))
                              :w (* 2 (:r this))
                              :h (* 2 (:r this))})
  (hit?   [this x y]        (<= (Math/sqrt
                                  (+ (Math/pow (- x (:x this)) 2)
                                     (Math/pow (- y (:y this)) 2)))
                                (:r this))))
```

After this, `(render a-square surf)` and `(render a-circle surf)`
both dispatch to the right implementation based on the type of the
first argument. Each protocol function dispatches on its first
argument's type.

> **v0.1 note.** `defrecord` is a v0.2 deliverable
> ([`STATUS.md`](../STATUS.md) D9). In v0.1 you use plain maps tagged
> with a `:type` keyword, and `extend-type` works against either a
> "class object" (a prototype, in protoCore terms) or a `Tag`
> sentinel. For v0.1, the idiomatic pattern is:
>
> ```clojure
> (defn square [side x y] {:type ::square :side side :x x :y y})
>
> (extend-type ::square
>   Renderable
>   (render [this surface] ...)
>   ...)
> ```

### 7.1.1 `extend-protocol` — extend many types to one protocol

The inverse of `extend-type`. When you have a protocol and want to
attach it to several types in one go:

```clojure
(extend-protocol Renderable
  ::square   ...impls...
  ::circle   ...impls...
  ::polygon  ...impls...)
```

Functionally identical to a series of `extend-type` calls; reads
better when several types share a protocol.

### 7.1.2 Protocols are open in one direction

You can declare a new protocol and extend it to a type the original
author never knew about. **That is the leverage point.** Imagine
protoPython exposes a `numpy.ndarray` to your Clojure code. You
want to teach Clojure code that `ndarray` is `Renderable`:

```clojure
(:require [py/numpy :as np])

(extend-protocol Renderable
  np/ndarray
  (render [this surface] (render-array surface this))
  (bounds [this]          (let [[h w] (py->clj (np/shape this))]
                            {:x 0 :y 0 :w w :h h}))
  (hit?   [this x y]      (and (< -1 x (:w (bounds this)))
                                (< -1 y (:h (bounds this))))))
```

Now every Clojure function that depends on the `Renderable` protocol
works on NumPy arrays. The Python authors did not have to do
anything; you extended their type from your code. This is the
"expression problem" solved.

(Whether `extend-type` against a foreign prototype is supported on
v0.1 depends on protoCore's mutable-prototype permissions; the v0.1
target is "yes for foreign mutables, no for primitive immediates
like SmallInteger". Updated in [`STATUS.md`](../STATUS.md) as the
implementation lands.)

### 7.1.3 The substrate makes this fast

A protocol method call lowers to:

```
get the receiver's protocol-method attribute (one attribute walk)
apply it to the args
```

That is one `getAttribute` chain walk and one call. The same path
protoST uses for every message send. The dispatch cost is the same
as any unary send on protoCore — competitive with JVM virtual
dispatch, sometimes faster because there is no interface vtable
lookup.

## 7.2 Multimethods

Multimethods are the more flexible, slower-dispatch alternative.
The dispatch is by an *arbitrary function of the args*, not by
receiver type.

```clojure
(defmulti area
  "Compute the area of a shape."
  :type)                              ;; dispatch on the :type key

(defmethod area :square [s]   (* (:side s) (:side s)))
(defmethod area :circle [c]   (* Math/PI (:r c) (:r c)))
(defmethod area :rectangle [r] (* (:w r) (:h r)))

(area {:type :square :side 4})        ;; => 16
(area {:type :circle :r 3})           ;; => 28.27...
```

The form `(defmulti name dispatch-fn)` creates a multimethod. When you
call it, Clojure:

1. Calls the dispatch function on the args. Here, `(:type x)` returns
   `:square` / `:circle` / `:rectangle`.
2. Looks the result up in the method table.
3. Calls the matching implementation.

The dispatch function is any function. It can look at multiple args:

```clojure
(defmulti collide
  "Resolve a collision between two shapes."
  (fn [a b] [(:type a) (:type b)]))    ;; dispatch on a pair of types

(defmethod collide [:square :square]   [a b] ...)
(defmethod collide [:square :circle]   [a b] ...)
(defmethod collide [:circle :square]   [a b] ...)
(defmethod collide [:circle :circle]   [a b] ...)
```

That is **double dispatch** — selecting the implementation based on
the types of *two* arguments. No Java mechanism does this directly;
you usually implement it with the visitor pattern. In Clojure it is
a one-line `defmulti`.

### 7.2.1 The default method

If no dispatch value matches, the multimethod falls through to a
`:default` method, if defined:

```clojure
(defmethod area :default [x]
  (throw (ex-info "Unknown shape" {:shape x})))
```

Without a default, an unmatched call raises `No method in multimethod`.

### 7.2.2 Hierarchies — `derive` and `isa?`

The neat trick: dispatch values can have a hierarchy, and lookup
falls back through it.

```clojure
(derive ::square     ::quadrilateral)
(derive ::rectangle  ::quadrilateral)
(derive ::trapezoid  ::quadrilateral)
(derive ::circle     ::round)

(defmulti describe :type)
(defmethod describe ::quadrilateral [_] "I have four sides.")
(defmethod describe ::round         [_] "I am round.")

(describe {:type ::square})            ;; => "I have four sides."
(describe {:type ::rectangle})         ;; => "I have four sides."
(describe {:type ::circle})            ;; => "I am round."
```

A method registered for `::quadrilateral` matches any value derived
from it. The hierarchy is global by default (`isa?` and `derive` use
a global hierarchy held in a var); custom hierarchies (`make-hierarchy`,
the third arg to `defmulti`) are supported but rarely needed.

When two ancestors of a dispatch value have competing methods, you
get an ambiguity error. `prefer-method` declares the resolution:

```clojure
(prefer-method describe ::round ::quadrilateral)
```

### 7.2.3 When to choose `defmulti` over `defprotocol`

Use a **protocol** when:

- Dispatch is by type of the first argument.
- You need fast dispatch (hot inner loop, ~ million calls/sec).
- The set of methods is small and bounded.

Use a **multimethod** when:

- Dispatch depends on more than one argument's type, or on a value, or
  on a computed key.
- The dispatch logic itself might evolve over time.
- Performance is not the bottleneck.

Most idiomatic Clojure code uses protocols for the polymorphic core
and multimethods for the edges where flexibility matters.

## 7.3 An example combining both

A small rendering pipeline. The `Renderable` protocol gives every
shape a `render` method. A `defmulti` picks the *transform* applied
to the shape before rendering, based on a runtime configuration:

```clojure
(defprotocol Renderable
  (render [this surface]))

(extend-protocol Renderable
  ::square (render [this surf] (draw-square surf this))
  ::circle (render [this surf] (draw-circle surf this)))

(defmulti transform
  "Apply a transform to a shape, returning a new shape."
  (fn [shape transform-spec] (:kind transform-spec)))

(defmethod transform :scale [shape {:keys [factor]}]
  (-> shape
      (update :side  * factor)
      (update :r     * factor)))

(defmethod transform :translate [shape {:keys [dx dy]}]
  (-> shape
      (update :x + dx)
      (update :y + dy)))

(defn pipeline [shape surface transforms]
  (-> (reduce transform shape transforms)
      (render surface)))

(pipeline
  {:type ::square :side 10 :x 0 :y 0}
  some-surface
  [{:kind :scale     :factor 2}
   {:kind :translate :dx 5 :dy 10}])
```

Each transformation kind is open — a new `:rotate` kind is a
`defmethod` away. Each shape type is open — a new `::polygon` is an
`extend-protocol` clause away. No class hierarchy, no factory
patterns, no visitors.

## 7.4 The substrate, briefly

For the curious:

- **Protocols** install their methods as attributes on the
  receiver-type prototype object. Dispatch is the standard protoCore
  `getAttribute` chain walk.
- **Multimethods** carry a `ProtoSparseList` mapping dispatch value →
  implementation, plus a reference to the global hierarchy
  (`#'clojure.core/global-hierarchy`). Dispatch is a sparse-list
  lookup with `isa?`-based fallback.
- A protocol method call is **fast** — one attribute walk. A
  multimethod call is **slower** — at minimum a hash lookup; with
  hierarchy involvement, a small graph walk.

The performance gap matters in inner loops. Outside them, both are
plenty fast.

Next: [Chapter 8 — Modules](08-modules.md).
