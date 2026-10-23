;; EXPECT: (user/a (clojure.core/seq (clojure.core/concat (clojure.core/list (quote user/b)) (clojure.core/list user/c))))
;; A nested syntax-quote is expanded first and its expansion quoted as data:
;; the inner ~c is not evaluated by the outer one, so c stays a symbol
;; (qualified, as Clojure's reader qualifies it).
(def c 99)
(println `(a `(b ~c)))
