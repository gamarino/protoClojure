;; EXPECT: (a (clojure.core/seq (clojure.core/concat (clojure.core/list (quote b)) (clojure.core/list c))))
;; A nested syntax-quote is expanded first and its expansion quoted as data:
;; the inner ~c is not evaluated by the outer one, so c stays a symbol.
(def c 99)
(println `(a `(b ~c)))
