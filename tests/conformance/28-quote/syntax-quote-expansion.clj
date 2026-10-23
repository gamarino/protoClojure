;; EXPECT: (clojure.core/seq (clojure.core/concat (clojure.core/list (quote user/f)) (clojure.core/list x) xs))
;; Syntax-quote expands at read time, as Clojure's reader does, and qualifies
;; the symbol `f`, which names no built-in, as `user/f`.
(println '`(f ~x ~@xs))
