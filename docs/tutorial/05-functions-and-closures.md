# 5. Functions and Closures

Functions are values. They have a name (sometimes), an arglist, and a
body. They close over the lexical environment where they were defined.
They are called with `(f args)`. Everything else in this chapter is
detail.

## 5.1 `defn` — the workhorse

```clojure
(defn square [x]
  (* x x))

(square 5)              ;; => 25
```

`defn name [args] body` interns a var named `name` in the current
namespace and points it at a function whose arglist is `[args]` and
whose body is `body`. The body's last expression is the return value;
there is no `return`.

A docstring goes between the name and the arglist; the conventional
formatting puts the arglist on a new line for multi-line bodies:

```clojure
(defn square
  "Return the square of x."
  [x]
  (* x x))
```

The docstring is accessible at runtime through `(doc square)` at the
REPL. Use it. The discipline matters.

## 5.2 Anonymous functions — `fn` and `#(...)`

Two forms, identical in semantics:

```clojure
(fn [x] (* x 2))          ;; anonymous function
#(* % 2)                  ;; reader shorthand: % is first arg
```

The shorthand uses `%`, `%1`–`%9`, and `%&` (rest):

```clojure
#(+ %1 %2)                ;; ≡ (fn [a b] (+ a b))
#(apply + %&)             ;; ≡ (fn [& xs] (apply + xs))
```

Pick `#(...)` when the body is small and the arg appears one or two
times. Pick `(fn [x] ...)` when the arg is named meaningfully or the
body is long enough that a name helps. Both styles are idiomatic;
preference is a question of readability.

A common rookie trap: `#(...)` does NOT nest. `#(...#(...)...)` is a
reader error. Use `(fn ...)` for the inner one.

## 5.3 Multi-arity

A function can have several arities. Each is a `([args] body)` clause:

```clojure
(defn greet
  ([]       (greet "world"))
  ([who]    (str "hello " who))
  ([greet w] (str greet " " w)))

(greet)                   ;; => "hello world"
(greet "alice")           ;; => "hello alice"
(greet "hi" "bob")        ;; => "hi bob"
```

The arity-0 calls arity-1 with a default. This is the canonical way to
give a function a default value — Clojure has no Python-style
`(x=default)` syntax, but the multi-arity is just as clean.

## 5.4 Variadic — `&`

A `&` in the arglist collects all remaining positional args into a seq:

```clojure
(defn sum [& xs]
  (reduce + 0 xs))

(sum 1 2 3 4)             ;; => 10
(sum)                     ;; => 0
```

You can mix required args and variadic:

```clojure
(defn log [level & msgs]
  (println (str "[" level "]") (apply str msgs)))

(log :info "user " "alice" " logged in")
;; [info] user alice logged in
```

`apply` is the inverse of `&` — it takes a function and a seq and calls
the function with the seq's elements as positional args.

```clojure
(apply + [1 2 3 4])       ;; => 10  — same as (+ 1 2 3 4)
(apply str ["hi" " " "there"])
                           ;; => "hi there"
```

## 5.5 Closures

Functions close over the lexical environment in which they are
*defined*. The classic counter-maker:

```clojure
(defn make-adder [n]
  (fn [x] (+ x n)))

(def add3 (make-adder 3))
(add3 10)                 ;; => 13
(add3 20)                 ;; => 23
```

`add3` is the inner `(fn [x] (+ x n))` with `n` bound to `3` from the
enclosing `make-adder` call. Calling `(make-adder 5)` again produces a
*different* function with `n` bound to `5`. Each closure is its own.

Captured locals are captured **by value at closure-creation time**.
They are immutable; the closure cannot modify them. If you need a
genuinely mutable cell — say, a real counter — you reach for an
`atom`:

```clojure
(defn make-counter []
  (let [n (atom 0)]
    (fn []
      (swap! n inc)
      @n)))

(def tick (make-counter))
(tick)                    ;; => 1
(tick)                    ;; => 2
(tick)                    ;; => 3
```

That is exactly the protoCore CAS primitive underneath. Atoms get a
whole chapter ([Chapter 6](06-state-and-atoms.md)).

## 5.6 `let` — local bindings inside a function

`let` introduces names visible only in its body:

```clojure
(defn hypotenuse [a b]
  (let [a2 (* a a)
        b2 (* b b)]
    (Math/sqrt (+ a2 b2))))
```

Bindings are sequential — `a2` is visible to `b2`'s right-hand side. A
let with many bindings reads top-to-bottom, like an assignment chain
in Python or JavaScript.

`let` can destructure (§5.10), which is one of its main payoffs:

```clojure
(let [[x y z] [1 2 3]]    ;; vector destructure
  (+ x y z))              ;; => 6

(let [{:keys [name age]} {:name "alice" :age 30}]
  (str name " is " age))  ;; => "alice is 30"
```

## 5.7 Recursion and `recur`

A function that calls itself in the tail position can use `recur` to
loop without growing the call stack:

```clojure
(defn factorial [n]
  (loop [n n
         acc 1]
    (if (<= n 1)
      acc
      (recur (dec n) (* acc n)))))

(factorial 20)            ;; => 2432902008176640000
```

Two pieces:

- `loop` introduces a *recur target* with initial bindings. It looks
  like `let` but it is also a jump label.
- `recur` jumps back to the nearest enclosing `loop` (or `fn`) with
  new bindings.

`recur` can also target the function itself:

```clojure
(defn count-up [n]
  (if (zero? n)
    :done
    (recur (dec n))))

(count-up 1000000)        ;; => :done   — no stack overflow
```

The compiler **enforces** that `recur` is in tail position. If you try
to use `recur` somewhere it cannot work, you get a clear compile error:

```clojure
(defn bad [n]
  (+ 1 (recur (dec n))))   ;; compile error: recur not in tail position
```

This is intentional. JVM Clojure does the same. Without explicit
`recur`, deep general recursion will eventually overflow the operand
stack — protoCore does not optimise tail calls automatically. That is
a real constraint and an honest one.

If your recursion is *not* tail and the input is bounded — say, walking
a tree — just use plain recursion. The stack handles thousands of frames
easily. Reach for `recur` when the recursion is iterative-shaped.

## 5.8 Higher-order functions

Functions are values. You pass them around, return them, store them.
The classic toolkit lives in `clojure.core`:

```clojure
(map inc [1 2 3])                ;; => (2 3 4)
(filter even? [1 2 3 4])         ;; => (2 4)
(reduce + 0 [1 2 3 4])           ;; => 10
(reduce conj [] '(1 2 3))        ;; => [1 2 3]  — accumulating into a vector
```

The combinator family:

```clojure
(comp inc str)                   ;; => function that does (inc (str x))
((comp inc count) "hello")       ;; => 6  — count of "hello", then inc

(partial + 10)                   ;; => function that does (+ 10 x ...)
((partial + 10) 5)               ;; => 15

(juxt inc dec)                   ;; => function returning [(inc x) (dec x)]
((juxt inc dec) 5)               ;; => [6 4]

(complement even?)               ;; => function that returns (not (even? x))
((complement even?) 3)           ;; => true
```

## 5.9 Lazy sequences — the seq returned by `map`

A subtle but important detail: most of the higher-order functions
return **lazy sequences**.

```clojure
(def squares (map #(* % %) (range)))   ;; squares of all naturals — lazy

(take 5 squares)                        ;; => (0 1 4 9 16)
```

`(range)` with no arguments is an infinite sequence; `map` on top of it
is also infinite. Lazy means values are produced on demand and memoised
after first realisation. `take 5` asks for five; you get five; the rest
never runs.

Two consequences worth knowing:

1. **A lazy seq with side effects is a trap.** If you `map` a function
   with side effects over a sequence, the side effects do not happen
   until something forces the seq. `doseq` is the side-effect-friendly
   iteration:

   ```clojure
   ;; Wrong — side effects may not happen
   (map println [1 2 3])

   ;; Right — eager iteration for side effects
   (doseq [x [1 2 3]]
     (println x))
   ```

2. **A lazy seq holds onto its head.** If you take the first element
   and store the seq in an atom, the whole tail you have realised so
   far is retained. For long-running streams, `take` early and discard
   the original.

A v0.1 note: protoClojure's lazy seqs are element-at-a-time. JVM Clojure
realises 32 elements at a time for performance ("chunked sequences").
Functionally identical; protoClojure is just slower per-element on
sequence-heavy code in v0.1. Chunked seqs land in v0.2. See
[`STATUS.md`](../STATUS.md) D4.

## 5.10 Destructuring

The pattern-matching shorthand. Lets `let`, `fn`, and `defn` pull apart
a value as you bind it:

**Vector destructuring** — positional:

```clojure
(let [[a b c] [1 2 3]]
  (+ a b c))                          ;; => 6

(let [[a b & rest] [1 2 3 4 5]]
  rest)                               ;; => (3 4 5)

(defn first-two [[a b]]
  [a b])

(first-two [10 20 30])                ;; => [10 20]
```

**Map destructuring** — named:

```clojure
(let [{name :name, age :age} {:name "alice" :age 30}]
  (str name " " age))                 ;; => "alice 30"

;; The :keys shorthand — when the binding name equals the keyword:
(let [{:keys [name age]} {:name "alice" :age 30}]
  (str name " " age))                 ;; => "alice 30"

(defn greet [{:keys [name greeting]
              :or {greeting "hi"}}]   ;; default for missing key
  (str greeting ", " name))

(greet {:name "alice"})               ;; => "hi, alice"
(greet {:name "bob" :greeting "hola"})
                                       ;; => "hola, bob"
```

Destructuring nests:

```clojure
(let [{:keys [name]
       {:keys [city zip]} :address}
      {:name "alice" :address {:city "BA" :zip 1428}}]
  (str name " lives in " city))       ;; => "alice lives in BA"
```

This is where the "data over methods" philosophy shows its hand.
Functions take maps and pull out what they need, without ceremony.

## 5.11 Pre/post conditions — a fast contract check

A function can declare runtime assertions on its inputs and outputs:

```clojure
(defn divide [a b]
  {:pre  [(number? a) (number? b) (not= 0 b)]
   :post [(number? %)]}                       ;; % is the return value
  (/ a b))

(divide 10 2)             ;; => 5
(divide 10 0)             ;; throws AssertionError
```

Use these for documentation as much as enforcement. They are checked
at runtime and can be turned off globally (a v0.2 toggle).

## 5.12 What we have not covered

- **Metadata on functions** — `^:dynamic`, `^:private`, custom
  attributes. Covered briefly in [`LANGUAGE.md`](../LANGUAGE.md).
- **`defn-`** for private functions in a namespace.
- **Function composition with transducers** — v0.2 deliverable.
- **`memoize`** — wraps a function so that calls cache by argument.
  Available in v0.1 core.

Next: [Chapter 6 — State and atoms](06-state-and-atoms.md).
