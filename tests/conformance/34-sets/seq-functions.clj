;; EXPECT: (2) 5 nil [7] 6 (1 2) 6 9 :a
;; Sets are seqable: map, first, seq, vec, apply, filter (with a set as the
;; predicate), reduce and doseq walk their elements; a keyword looks itself
;; up in a set.
(def out (atom nil))
(doseq [x #{9}] (reset! out x))
(println (map inc #{1}) (first #{5}) (seq #{}) (vec #{7}) (apply + #{1 2 3})
         (filter #{1 2} [1 2 3 4]) (reduce + #{1 2 3}) @out (:a #{:a :b}))
