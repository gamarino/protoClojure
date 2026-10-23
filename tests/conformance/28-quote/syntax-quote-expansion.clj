;; EXPECT: (clojure.core/seq (clojure.core/concat (clojure.core/list (quote f)) (clojure.core/list x) xs))
;; Syntax-quote expands at read time, as Clojure's reader does. Clojure also
;; qualifies `f` as `user/f`; protoClojure does not yet (D35).
(println '`(f ~x ~@xs))
