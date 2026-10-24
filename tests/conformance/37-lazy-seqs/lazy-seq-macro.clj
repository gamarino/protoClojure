;; EXPECT: (0 1 1 2 3 5 8 13 21 34)
;; lazy-seq defers its body; a recursive lazy-seq is an endless sequence.
(defn fib-seq
  ([] (fib-seq 0 1))
  ([a b] (lazy-seq (cons a (fib-seq b (+ a b))))))
(println (take 10 (fib-seq)))
