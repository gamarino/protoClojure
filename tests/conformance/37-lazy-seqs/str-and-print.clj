;; EXPECT: clojure.lang.LazySeq@ (2) (1 2) (0 1 2)
;; str renders a lazy seq as JVM Clojure does (its class and a hash), pr-str
;; and println print its elements; a cons prints as a list.
(println (subs (str (map inc [1])) 0 21) (pr-str (map inc [1])) (str (cons 1 '(2))) (range 3))
