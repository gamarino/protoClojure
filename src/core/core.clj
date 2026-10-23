;; core.clj -- the part of clojure.core protoClojure writes in Clojure.
;;
;; Loaded at start-up, before the program, by every driver (a script, the
;; REPL, eval's callers); it is embedded in the protoclj binary at build time
;; (src/compiler/Eval.cpp, CMakeLists.txt), so nothing is installed beside
;; it. While it loads, the current namespace is clojure.core: every
;; definition is installed under both `x` and `clojure.core/x`, and
;; syntax-quote qualifies the names in its templates as clojure.core/x
;; (docs/DESIGN.md §10.1, M20).
;;
;; Names that start with `__` are internal helpers, not part of Clojure.
;;
;; The macros follow clojure.core's definitions, without what protoClojure
;; does not have yet (metadata, lazy seqs). Binding forms destructure: the
;; binding macros expand into `let`, which the compiler destructures.
;; `and`, `or`, `fn`, `let`, `loop`, `defn` and `defmacro` stay in the C++
;; compiler (decision M21).

;; ---- Small functions the macros and programs use ------------------------

(defn identity [x] x)
(defn some? [x] (not (nil? x)))
(defn true? [x] (= x true))
(defn false? [x] (= x false))
(defn zero? [n] (= n 0))
(defn pos? [n] (> n 0))
(defn neg? [n] (< n 0))
(defn nnext [coll] (next (next coll)))
(defn ffirst [coll] (first (first coll)))
(defn nthnext [coll n]
  (loop [n n xs (seq coll)]
    (if (if xs (> n 0) false) (recur (- n 1) (next xs)) xs)))
(defn butlast [coll]
  (loop [acc [] xs (seq coll)]
    (if (next xs) (recur (conj acc (first xs)) (next xs)) (seq acc))))

(defn into
  "Returns a new coll consisting of to-coll with all of the items of
  from-coll conjoined."
  ([] [])
  ([to] to)
  ([to from] (reduce conj to from)))

;; ---- Control flow --------------------------------------------------------

(defmacro when
  "Evaluates test. If logical true, evaluates body in an implicit do."
  [test & body]
  (list 'if test (cons 'do body)))

(defmacro when-not
  "Evaluates test. If logical false, evaluates body in an implicit do."
  [test & body]
  (list 'if test nil (cons 'do body)))

(defmacro if-not
  "Evaluates test. If logical false, evaluates and returns then expr,
  otherwise else expr, if supplied, else nil."
  ([test then] `(if-not ~test ~then nil))
  ([test then else] `(if (not ~test) ~then ~else)))

(defmacro cond
  "Takes a set of test/expr pairs. It evaluates each test one at a time.
  If a test returns logical true, cond evaluates and returns the value of
  the corresponding expr and doesn't evaluate any of the other tests or
  exprs. (cond) returns nil."
  [& clauses]
  (when clauses
    (list 'if (first clauses)
          (if (next clauses)
            (second clauses)
            (__raise-illegal-argument "cond requires an even number of forms"))
          (cons 'clojure.core/cond (next (next clauses))))))

(defmacro comment
  "Ignores body, yields nil."
  [& body] nil)

(defmacro while
  "Repeatedly executes body while test expression is true."
  [test & body]
  `(loop [] (when ~test ~@body (recur))))

;; ---- Binding conditionals -------------------------------------------------

(defn __check-binding [macro bindings]
  (when-not (vector? bindings)
    (__raise-illegal-argument (str macro " requires a vector for its binding")))
  (when-not (= 2 (count bindings))
    (__raise-illegal-argument (str macro " requires exactly 2 forms in binding vector"))))

(defmacro if-let
  "bindings => binding-form test. If test is true, evaluates then with
  binding-form bound to the value of test, if not, yields else."
  ([bindings then] `(if-let ~bindings ~then nil))
  ([bindings then else]
   (__check-binding "if-let" bindings)
   `(let [temp# ~(second bindings)]
      (if temp#
        (let [~(first bindings) temp#] ~then)
        ~else))))

(defmacro when-let
  "bindings => binding-form test. When test is true, evaluates body with
  binding-form bound to the value of test."
  [bindings & body]
  (__check-binding "when-let" bindings)
  `(let [temp# ~(second bindings)]
     (when temp#
       (let [~(first bindings) temp#] ~@body))))

(defmacro if-some
  "bindings => binding-form test. If test is not nil, evaluates then with
  binding-form bound to the value of test, if not, yields else."
  ([bindings then] `(if-some ~bindings ~then nil))
  ([bindings then else]
   (__check-binding "if-some" bindings)
   `(let [temp# ~(second bindings)]
      (if (nil? temp#)
        ~else
        (let [~(first bindings) temp#] ~then)))))

(defmacro when-some
  "bindings => binding-form test. When test is not nil, evaluates body with
  binding-form bound to the value of test."
  [bindings & body]
  (__check-binding "when-some" bindings)
  `(let [temp# ~(second bindings)]
     (if (nil? temp#)
       nil
       (let [~(first bindings) temp#] ~@body))))

;; ---- Threading -----------------------------------------------------------

(defn __thread-first [x form]
  (if (seq? form) `(~(first form) ~x ~@(next form)) (list form x)))

(defn __thread-last [x form]
  (if (seq? form) `(~(first form) ~@(next form) ~x) (list form x)))

(defmacro ->
  "Threads the expr through the forms. Inserts x as the second item in the
  first form, making a list of it if it is not a list already."
  [x & forms]
  (loop [x x forms forms]
    (if forms (recur (__thread-first x (first forms)) (next forms)) x)))

(defmacro ->>
  "Threads the expr through the forms. Inserts x as the last item in the
  first form, making a list of it if it is not a list already."
  [x & forms]
  (loop [x x forms forms]
    (if forms (recur (__thread-last x (first forms)) (next forms)) x)))

(defmacro as->
  "Binds name to expr, evaluates the first form in the lexical context of
  that binding, then binds name to that result, repeating for each
  successive form, returning the result of the last form."
  [expr name & forms]
  `(let [~name ~expr
         ~@(reduce (fn [acc form] (conj (conj acc name) form)) [] forms)]
     ~name))

(defn __steps [g forms step]
  (reduce (fn [acc form] (conj (conj acc g) (step form))) [] forms))

(defmacro some->
  "When expr is not nil, threads it into the first form (via ->), and when
  that result is not nil, through the next etc."
  [expr & forms]
  (let [g (gensym)]
    `(let [~g ~expr
           ~@(__steps g forms (fn [f] `(if (nil? ~g) nil (-> ~g ~f))))]
       ~g)))

(defmacro some->>
  "When expr is not nil, threads it into the first form (via ->>), and when
  that result is not nil, through the next etc."
  [expr & forms]
  (let [g (gensym)]
    `(let [~g ~expr
           ~@(__steps g forms (fn [f] `(if (nil? ~g) nil (->> ~g ~f))))]
       ~g)))

(defn __cond-steps [g clauses thread]
  (loop [acc [] cs (seq clauses)]
    (if cs
      (recur (conj (conj acc g) `(if ~(first cs) (~thread ~g ~(second cs)) ~g))
             (nnext cs))
      acc)))

(defmacro cond->
  "Takes an expression and a set of test/form pairs. Threads expr (via ->)
  through each form for which the corresponding test expression is true."
  [expr & clauses]
  (when (odd? (count clauses))
    (__raise-illegal-argument "cond-> requires an even number of forms"))
  (let [g (gensym)]
    `(let [~g ~expr ~@(__cond-steps g clauses 'clojure.core/->)] ~g)))

(defmacro cond->>
  "Takes an expression and a set of test/form pairs. Threads expr (via ->>)
  through each form for which the corresponding test expression is true."
  [expr & clauses]
  (when (odd? (count clauses))
    (__raise-illegal-argument "cond->> requires an even number of forms"))
  (let [g (gensym)]
    `(let [~g ~expr ~@(__cond-steps g clauses 'clojure.core/->>)] ~g)))

(defmacro doto
  "Evaluates x then calls all of the methods and functions with the value
  of x supplied at the front of the given arguments. Returns x."
  [x & forms]
  (let [g (gensym)]
    `(let [~g ~x]
       ~@(map (fn [f] (if (seq? f) `(~(first f) ~g ~@(next f)) `(~f ~g))) forms)
       ~g)))

;; ---- Iteration -----------------------------------------------------------

(defmacro dotimes
  "bindings => name n. Repeatedly executes body (presumably for side
  effects) with name bound to integers from 0 through n-1."
  [bindings & body]
  (__check-binding "dotimes" bindings)
  (let [i (first bindings)]
    `(let [n# ~(second bindings)]
       (loop [~i 0]
         (when (< ~i n#)
           ~@body
           (recur (+ ~i 1)))))))

(defmacro doseq
  "Repeatedly executes body (presumably for side effects) with bindings,
  nested left to right, bound to each element of their sequences. Returns
  nil."
  [bindings & body]
  (when-not (vector? bindings)
    (__raise-illegal-argument "doseq requires a vector for its binding"))
  (when (odd? (count bindings))
    (__raise-illegal-argument "doseq requires an even number of forms in binding vector"))
  (if (seq bindings)
    (let [more (nnext bindings)]
      `(loop [s# (seq ~(second bindings))]
         (when s#
           (let [~(first bindings) (first s#)]
             ~(if more `(doseq ~(vec more) ~@body) `(do ~@body)))
           (recur (next s#)))))
    `(do ~@body nil)))

;; ---- Dispatch ------------------------------------------------------------

(defn __case-test [g constant]
  (if (seq? constant)
    (cons 'clojure.core/or (map (fn [c] `(= ~g '~c)) constant))
    `(= ~g '~constant)))

(defn __case-clauses [g clauses]
  (cond
    (nil? clauses) `(__raise-illegal-argument (str "No matching clause: " (pr-str ~g)))
    (nil? (next clauses)) (first clauses)
    :else `(if ~(__case-test g (first clauses))
             ~(second clauses)
             ~(__case-clauses g (nnext clauses)))))

(defmacro case
  "Takes an expression, and a set of clauses. Each clause is test-constant
  result-expr, or (test-constant1 ... test-constantN) result-expr, and an
  optional last default expression. The test-constants are not evaluated.
  Without a default and a matching constant, throws
  IllegalArgumentException."
  [e & clauses]
  (let [g (gensym "case__")]
    `(let [~g ~e] ~(__case-clauses g (seq clauses)))))

(defn __condp-clauses [gpred gexpr clauses]
  (cond
    (nil? clauses) `(__raise-illegal-argument (str "No matching clause: " (pr-str ~gexpr)))
    (nil? (next clauses)) (first clauses)
    (= :>> (second clauses))
    `(let [p# (~gpred ~(first clauses) ~gexpr)]
       (if p# (~(nth clauses 2) p#) ~(__condp-clauses gpred gexpr (nthnext clauses 3))))
    :else `(if (~gpred ~(first clauses) ~gexpr)
             ~(second clauses)
             ~(__condp-clauses gpred gexpr (nnext clauses)))))

(defmacro condp
  "Takes a binary predicate, an expression, and a set of clauses: test-expr
  result-expr, or test-expr :>> result-fn, and an optional default. For
  each clause, (pred test-expr expr) is evaluated; the first true one
  yields its result-expr, or (result-fn (pred test-expr expr)). Without a
  default and a true clause, throws IllegalArgumentException."
  [pred expr & clauses]
  (let [gpred (gensym "pred__") gexpr (gensym "expr__")]
    `(let [~gpred ~pred ~gexpr ~expr]
       ~(__condp-clauses gpred gexpr (seq clauses)))))

;; ---- Resources and threads -----------------------------------------------

(defmacro with-open
  "bindings => [name init ...]. Evaluates body in a try expression with
  names bound to the values of the inits, and a finally clause that closes
  each name, in reverse order."
  [bindings & body]
  (when-not (vector? bindings)
    (__raise-illegal-argument "with-open requires a vector for its binding"))
  (when (odd? (count bindings))
    (__raise-illegal-argument "with-open requires an even number of forms in binding vector"))
  (if (seq bindings)
    (do
      (when-not (symbol? (first bindings))
        (__raise-illegal-argument "with-open only allows Symbols in bindings"))
      `(let [~(first bindings) ~(second bindings)]
         (try
           (with-open ~(vec (nnext bindings)) ~@body)
           (finally (__with_open_close__ ~(first bindings))))))
    `(do ~@body)))

(defmacro future
  "Takes a body of expressions and yields a future object that will invoke
  the body in another thread."
  [& body]
  `(make-future (fn [] ~@body)))
