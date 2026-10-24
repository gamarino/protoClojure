;; EXPECT-ERROR: StackOverflowError
;; A lazy seq whose body reads the seq itself cannot finish (JVM Clojure:
;; StackOverflowError).
(def s (lazy-seq (cons 1 (seq s))))
(println (first s))
