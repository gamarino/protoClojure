# 8. Modules

A protoClojure program is a tree of files, each declaring a
**namespace** and listing what it needs from elsewhere. The mechanics
look almost identical to Clojure-JVM — the same `ns` form, the same
`:require`, the same `:as` and `:refer`. Underneath, every require
goes through the **UMD module resolver** the rest of the protoCore
ecosystem already uses. That hybrid is the next chapter's subject;
this one is about plain Clojure-on-Clojure modules.

## 8.1 The `ns` form

Every `.clj` file starts with one:

```clojure
(ns demo.greeting
  "A trivial greeting library."
  (:require [clojure.string :as str]
            [demo.util :refer [titlecase]]))

(defn greet [name]
  (str "hello, " (titlecase name)))
```

`(ns demo.greeting ...)`:

- Creates the namespace `demo.greeting` if it does not exist.
- Switches the current namespace to it.
- Processes the `:require` clauses to bring symbols into scope.

The optional docstring after the namespace name is what `(doc
demo.greeting)` returns at the REPL.

## 8.2 File names mirror namespace names

A namespace `demo.greeting` lives in a file `demo/greeting.clj` on the
**CLOJURE_PATH**. Dots in the namespace become slashes in the path;
the segments before the final segment are directories.

```
project/
├── demo/
│   ├── greeting.clj         ← (ns demo.greeting ...)
│   ├── util.clj             ← (ns demo.util ...)
│   └── http/
│       └── server.clj       ← (ns demo.http.server ...)
└── main.clj                  ← (ns main ...)
```

If the namespace contains a dash, the filename uses an underscore:
`my-app.core` lives in `my_app/core.clj`. This is a JVM Clojure
inheritance and we keep it because Clojurists' fingers expect it.

## 8.3 `CLOJURE_PATH`

The colon-separated list of directories the resolver walks. Set it
once per shell, or per project:

```bash
export CLOJURE_PATH=src:resources:/usr/share/protoclj
```

The current source file's directory is always implicitly on the path.
That means a script in `/tmp/play.clj` can `(:require [my.helper])`
and pick up `/tmp/my/helper.clj` without configuration. Useful for
quick prototypes.

For a real project, put `src/` on the path and lay namespaces out
under it.

## 8.4 `:require` — the workhorse

Every cross-namespace dependency goes through `:require`. The
common forms:

```clojure
(:require [clojure.string :as str])
                                      ;; str/upper-case, str/split, …
(:require [clojure.string :as str :refer [join]])
                                      ;; join works without prefix; str/anything else
(:require [demo.util :refer [helper-1 helper-2]])
                                      ;; bring named symbols directly
(:require [demo.util :refer :all])
                                      ;; bring everything public — discouraged
(:require [demo.util])
                                      ;; load the namespace, no aliases
                                      ;; (you can refer to demo.util/x explicitly)
(:require [demo.config :rename {load load-config}])
                                      ;; rename a refer to avoid a collision
```

`:as` and `:refer` can combine in one map. The full grammar lives in
[`LANGUAGE.md`](../LANGUAGE.md) §7; the four forms above cover 99% of
real code.

The conventional ordering inside `:require` clauses, alphabetical by
namespace, with cross-runtime prefixed last:

```clojure
(ns my.app
  (:require [clojure.string :as str]
            [clojure.set :as set]
            [my.util :refer [fmt]]
            [my.db :as db]
            [py/numpy :as np]
            [js/lodash :as _]))
```

Style only; the resolver does not care about order.

## 8.5 Vars and namespaces

Every `def` interns a **var** in the current namespace. A var is a
name; it points at a value but can be rebound (the value can change,
the binding can change). Code that uses the symbol resolves to the var,
not the value directly — so when you redefine a function at the
REPL, every caller picks up the new definition without recompiling.

```clojure
(in-ns 'demo.greeting)

(def greeting "hello")              ;; interns var #'demo.greeting/greeting
(def ^:private secret "shh")        ;; ^:private hides from other ns's :require :refer
(def ^:dynamic *width* 80)          ;; dynamic for thread-local rebinding
```

The `#'name` syntax (var-quote) returns the *var itself*, not its
value. You rarely need to write it — most code calls vars through the
symbol — but it is the right tool for "give me the box, not the
contents":

```clojure
#'clojure.core/inc                   ;; the var, not the function
@#'clojure.core/inc                   ;; deref the var, get the function
((deref #'clojure.core/inc) 5)        ;; => 6
```

`alter-var-root` mutates a var's root binding globally:

```clojure
(alter-var-root #'my-app/config (constantly (load-config)))
```

This is the right tool for one-time application configuration and
the wrong tool for almost everything else. If you find yourself
reaching for it inside an inner loop, you want an atom.

## 8.6 `^:dynamic` and `binding`

A var marked `^:dynamic` can be **rebound** for a thread-local scope
with `binding`:

```clojure
(def ^:dynamic *width* 80)

(defn trim-line [s]
  (subs s 0 (min (count s) *width*)))

(trim-line "hello world")            ;; => "hello world"  (width 80)

(binding [*width* 5]
  (trim-line "hello world"))         ;; => "hello"        (width 5)

(trim-line "hello world")            ;; => "hello world"  (back to 80)
```

The convention is to surround dynamic var names with `*earmuffs*` so
they read as "this is dynamic" at the call site. `binding` pushes a
thread-local rebinding for the duration of its body; concurrent
threads see their own bindings; the original root value is unchanged
outside.

Use sparingly. Dynamic vars are excellent for ambient configuration
("the current log level", "the current output stream"), error-prone
for anything that participates in normal data flow.

## 8.7 Reloading and the REPL workflow

The Clojure REPL is `def` all the way down. Every time you re-evaluate
a `def` or `defn` at the REPL, the var is updated. Every caller —
already-running, queued, or future — picks up the new value the next
time it dereferences. *That is the magic of REPL-driven development*:
you can fix a bug while the program is running.

The mechanics:

```
;; At the REPL
user=> (defn handler [req] (str "version 1: " (:body req)))
#'user/handler

;; Some other code holds a reference to #'user/handler and dispatches to it.
;; You spot a bug.

user=> (defn handler [req] (str "version 2: " (:body req)))
#'user/handler

;; The next request the dispatcher handles uses version 2. No restart.
```

This is also why JVM Clojure people invest so much in their editor
setup. Sending a form from the editor to the REPL with one keystroke
is the workflow — not "save the file and restart".

Reloading a *file*:

```clojure
(require 'demo.greeting :reload)
;; or
(load-file "src/demo/greeting.clj")
```

`:reload` re-evaluates the namespace; `load-file` re-evaluates the
file. Either way, the vars are reinstated with the current source.

A subtle reloading gotcha: a `def` that is *removed* from the source
file is **not** removed from the namespace by `:reload`. The var
still exists, pointing at the old value. To clean up:

```clojure
(ns-unmap 'demo.greeting 'old-name)
```

Or just trust your editor's "tools.namespace" integration to handle
it (a v0.2 deliverable; until then, manual cleanup).

## 8.8 Private vars and the `defn-` shortcut

Mark a var private with `^:private` metadata to exclude it from `:as`
and `:refer :all` imports:

```clojure
(defn ^:private helper [x] ...)
```

The `defn-` macro is sugar for the same:

```clojure
(defn- helper [x] ...)
```

Private vars are still reachable from outside by var-quote (`@#'ns/helper`)
— Clojure does not enforce visibility, only suggests it. The convention
is a polite request to the reader.

## 8.9 What `clojure.core` brings

Every namespace implicitly `:refer`s the public vars of
`clojure.core` — `def`, `defn`, `fn`, `let`, `if`, all the
collection and higher-order functions, etc. You never write `(:require
[clojure.core])` because it is always there.

If you want to *shadow* a core function with your own, just `def` it
in your namespace; references in your namespace resolve to your
version. Other namespaces still see the core version unless they
explicitly `:refer` yours.

## 8.10 What is in scope inside a `defn`

The lookup order at every symbol reference inside a function body:

1. Local bindings (`let`, `fn` args, `loop` bindings, `for`
   bindings).
2. Vars interned in the current namespace.
3. Vars `:refer`'d into the current namespace.
4. `clojure.core` vars.

If none of those resolve, you get a clear `Unable to resolve symbol:
foo`. That error is your friend — it always points at the source
line.

## 8.11 An end-to-end example

A miniature application split over four files:

```
project/
└── src/
    ├── app/
    │   ├── core.clj
    │   ├── http.clj
    │   └── db.clj
    └── app/util.clj
```

**src/app/util.clj**
```clojure
(ns app.util)

(defn fmt-line
  "Format a log line."
  [level msg]
  (str "[" (name level) "] " msg))
```

**src/app/db.clj**
```clojure
(ns app.db
  (:require [app.util :refer [fmt-line]]))

(def ^:private connection (atom nil))

(defn connect! [url]
  (reset! connection {:url url :open? true})
  (println (fmt-line :info (str "connected to " url))))

(defn query [sql]
  (when-not (:open? @connection)
    (throw (ex-info "not connected" {:sql sql})))
  ;; pretend
  [:result-for sql])
```

**src/app/http.clj**
```clojure
(ns app.http
  (:require [app.util :refer [fmt-line]]
            [app.db :as db]))

(defn handle-request [{:keys [path]}]
  (println (fmt-line :info (str "handling " path)))
  (db/query (str "select * from " path)))
```

**src/app/core.clj**
```clojure
(ns app.core
  (:require [app.db :as db]
            [app.http :as http]))

(defn -main [& args]
  (db/connect! "memory://demo")
  (println (http/handle-request {:path "users"})))
```

Run:

```bash
$ CLOJURE_PATH=src protoclj src/app/core.clj
[info] connected to memory://demo
[info] handling users
[:result-for "select * from users"]
```

Every file is `(ns ... :require ...)` followed by `def`s and `defn`s.
That is the shape of every Clojure project, from a 50-line script to
a 50,000-line application. The same `ns` form scales without changing.

Next: [Chapter 9 — Cross-runtime interop](09-interop.md). The same
`:require` mechanism, but now the requirement might be a NumPy
module, a Lodash function, or a protoST class.
