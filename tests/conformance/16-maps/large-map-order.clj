;; EXPECT: {:a 1, :b 2, :c 3, :d 4, :e 5, :f 6, :g 7, :h 8, :i 9, :j 10}
;; protoClojure keeps insertion order at every size (deviation D19: the JVM
;; guarantees it only for array maps of at most 8 entries).
(println {:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8 :i 9 :j 10})
