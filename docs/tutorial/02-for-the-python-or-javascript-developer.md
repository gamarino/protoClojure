# 2. For the Python or JavaScript Developer

Clojure looks unfamiliar at first — there are a lot of parentheses and
the function comes *before* the arguments — but the underlying ideas
are smaller than they look. If you have written Python or JavaScript
for a while, you already work with most of them; Clojure just makes
them explicit. This chapter unpacks the surface so the rest of the
tutorial reads naturally.

## 2.1 The first surprise: the function comes first

In Python you write `f(x, y)`. In Clojure you write `(f x y)`.

```clojure
(+ 1 2)              ; → 3,   like 1 + 2
(* 3 4 5)            ; → 60,  like 3 * 4 * 5
(println "hello")    ; → prints "hello"
```

That is all the syntax change is, at first. The function moves inside
the parentheses, in the position you would otherwise use for the
operator. `+` is not special — it is a function whose name happens to
be the plus sign.

The reason this matters: **the syntax is uniform**. There is no
distinction between operators and functions, between methods and
functions, between built-ins and user code. The same shape covers
everything. Once you stop reaching for `f(...)` and write `(f ...)`,
you stop having to learn syntactic exceptions for every new language
feature — because there aren't any.

## 2.2 Data has literal syntax too

Python has `[1, 2, 3]` for a list and `{1, 2, 3}` for a set and
`{"a": 1}` for a dict. Clojure has all four with similar shapes:

```clojure
[1 2 3]              ; vector — like a Python list / JS array
'(1 2 3)             ; list   — like a Python list (with one twist: the quote, §2.5)
{:a 1 :b 2}          ; map    — like a Python dict / JS object
#{1 2 3}             ; set    — like a Python set / new Set()
```

Two differences from what you might expect:

- **No commas.** Commas are whitespace in Clojure. `[1, 2, 3]` is the
  same as `[1 2 3]`. Pick whatever reads best.
- **`:a` is a keyword, not a string.** Keywords are like Python's
  `Enum` values or JavaScript's `Symbol` — interned, fast to compare,
  used everywhere a Python programmer would reach for a string key
  in a dict. You will see them constantly.

## 2.3 Immutability — the genuinely new idea

This is the part of Clojure that takes a real shift. Try this:

```python
# Python
xs = [1, 2, 3]
xs.append(4)
# xs is now [1, 2, 3, 4]
```

The equivalent in Clojure:

```clojure
(def xs [1 2 3])
(conj xs 4)            ; → [1 2 3 4]
xs                     ; → [1 2 3]  — unchanged!
```

`conj` did not modify `xs`. It *returned a new vector* with `4`
appended. `xs` still points to the original. If you want the new
vector, you have to keep the return value:

```clojure
(def ys (conj xs 4))   ; ys = [1 2 3 4], xs = [1 2 3]
```

The same is true of `assoc` (set a key in a map), `dissoc` (remove a
key), `update` (apply a function to a value), and every other
"mutation" you might expect:

```clojure
(def m {:a 1 :b 2})
(assoc m :c 3)         ; → {:a 1, :b 2, :c 3}
m                       ; → {:a 1, :b 2}  — unchanged
```

You might worry this is impossibly slow. Building a new whole vector
for every append would be wasteful. The clever part is that Clojure's
collections are **persistent** — they share structure with the
original. `(conj xs 4)` does not copy the whole vector; it builds a
new vector that *shares* the underlying tree nodes with `xs` and
adds a small bit at the tail. The cost is `O(log n)` per operation —
slightly more than mutation, but well within the noise on real
workloads, and you get back the entire class of bugs caused by
shared mutable state.

The protoCore object kernel underneath is built around this — every
collection is structurally shared and immutable by default. Clojure
does not have to convince it; this is just how the substrate works.

## 2.4 Variables vs. names

A Python variable is a name that points to a value, and you can
reassign it. A Clojure name is the same — except you rarely reassign.
You make a new name with `def` (at the file top level) or `let`
(inside a function):

```clojure
(def pi 3.14159)                    ; like Python's pi = 3.14159

(let [x 10
      y 20
      z (+ x y)]
  (println z))                       ; prints 30
```

`let` binds local names, evaluates the body, and the bindings disappear
when the body ends. There is no `=` operator inside a function — you
introduce a new local with another `let`, often nested in a chain. This
sounds tedious; in practice it is liberating because every name in a
block has a fixed meaning.

If you genuinely need a mutable cell — a counter, a cache — Clojure
gives you an `atom` (Chapter 6). It is a deliberate, named tool, not
the default.

## 2.5 The quote

The one form that looks strange at first:

```clojure
(+ 1 2)        ; → 3      — calls + on 1 and 2
'(+ 1 2)       ; → (+ 1 2) — a list of three symbols
```

The leading `'` is the **quote**. It tells the reader "this next form is
data, don't evaluate it". Without the quote, `(+ 1 2)` is a function
call. With the quote, it's a three-element list whose elements happen
to be the symbol `+`, the number `1`, and the number `2`.

The reason this exists: in Clojure, **code is data**. A function call
is *literally* a list. The reader produces a list, the evaluator
walks the list and applies. Most of the time you want the evaluation,
and you write `(...)`. Sometimes you want the list itself — to
manipulate it, write a macro, store it for later — and you write `'(...)`.

This is the seed of macros (Chapter 11). For now, just know that the
quote means "this is data, don't evaluate it".

## 2.6 Functions

You write a function with `defn`:

```clojure
(defn square [x]
  (* x x))

(square 5)              ; → 25
```

This is exactly equivalent to:

```python
def square(x):
    return x * x
```

The differences:

- The argument list is in square brackets `[x]`, because square brackets
  mean "vector" everywhere in Clojure including here.
- There is no `return`. The last expression in the body is the return
  value. (`square` is one expression, so it is also the return value.)

Anonymous functions exist too:

```clojure
(fn [x] (* x x))                    ; equivalent to Python's lambda x: x * x
((fn [x] (* x x)) 5)                ; → 25
#(* % %)                            ; shorthand: % is the first argument
```

## 2.7 Higher-order functions

If you have written any functional-style Python or JavaScript, this
will feel familiar. `map`, `filter`, `reduce` work as you expect — and
because they receive function values as ordinary arguments, the syntax
is uniform:

```clojure
(map inc [1 2 3])              ; → (2 3 4),     like list(map(lambda x: x+1, [1,2,3]))
(filter even? [1 2 3 4 5])     ; → (2 4),       like list(filter(lambda x: x%2==0, [1,2,3,4,5]))
(reduce + 0 [1 2 3 4 5])       ; → 15,          like functools.reduce(lambda a,b: a+b, [1,2,3,4,5], 0)
```

Notice that the result of `map` is a sequence printed as `(...)`, not a
vector `[...]`. Clojure has a *seq* abstraction, which is what most
higher-order functions return — and most of the time you do not have
to think about it because anything you can do to a vector you can also
do to a seq.

When you specifically want a vector back, wrap in `vec` (or use `mapv`,
the eager vector form of `map`):

```clojure
(vec (map inc [1 2 3]))        ; → [2 3 4]
(mapv inc [1 2 3])             ; → [2 3 4]
```

## 2.8 The threading macros — Clojure's killer feature for readability

A common Python pattern:

```python
result = sorted(
    filter(lambda x: x > 0,
           map(lambda x: x * 2,
               data)),
    reverse=True)
```

Reads inside-out. Clojure has the same problem and solves it with
**threading macros**:

```clojure
(->> data
     (map #(* % 2))
     (filter pos?)
     (sort >))
```

The `->>` ("thread-last") macro takes the value on the left and threads
it as the *last* argument of each subsequent form. So `(->> data
(map f) (filter g))` rewrites at compile time to `(filter g (map f
data))`. You read top-to-bottom, which is how pipelines look in your
head anyway. There is also `->` ("thread-first") which threads as the
first argument; it's used with object-method-style calls.

Once you start reaching for these, you reach for them constantly.

## 2.9 The REPL

This is the part of Clojure programmers rave about, and it is real.

You launch `protoclj` and you get a prompt. Type a form, press enter,
get the result:

```
$ protoclj
user=> (+ 1 2)
3
user=> (def xs [10 20 30])
#'user/xs
user=> (map inc xs)
(11 21 31)
user=> (defn double [x] (* 2 x))
#'user/double
user=> (double 21)
42
```

Two things make this more than what Python's REPL gives you:

- **You can redefine anything at any time.** Functions, vars, even
  Clojure macros. The change takes effect for every caller from the
  next call onward. You build the program incrementally; you do not
  restart.
- **An editor with a CIDER / Calva / Conjure plugin sends every form
  you type to the REPL automatically.** You write your code in the
  editor, press a key, and the form runs in the live REPL session.
  Combined with the no-restart behaviour, this means you can rewrite
  a function, test it, fix a bug, and have the entire system updated
  without ever leaving the editor — and without losing any program
  state you built up.

This is what is meant by "REPL-driven development". It is the way
Clojure programmers actually work. protoClojure ships an nREPL server
in v0.1 specifically so that workflow works.

## 2.10 Where this is going

The rest of the tutorial fills in the details. The big ideas you now
have:

- **Function-first syntax**: `(f x y)` covers everything.
- **Immutable persistent collections**: `assoc`, `conj`, `update`
  return new values.
- **`let` for locals, `def` for top-level names**, `defn` for
  functions.
- **Code is data**, gated by the quote.
- **Threading macros**: `->>` for readability.
- **The REPL is how you actually work**.

Move on to [Chapter 4](04-data-structures.md) — the four collection
types in detail. (Chapter 3 is for readers coming from Clojure-JVM;
you can skip it.)
