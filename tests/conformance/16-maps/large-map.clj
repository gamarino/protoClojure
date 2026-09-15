;; EXPECT: 1 10 10 55
;; A map larger than protoCore's small sparse-list form keeps every entry.
(def m {:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8 :i 9 :j 10})
(println (:a m) (:j m) (count (keys m)) (reduce + (vals m)))
