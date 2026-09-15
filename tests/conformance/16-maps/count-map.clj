;; EXPECT: 0 2 3 3 2 10
;; count of a map is its number of entries: assoc of an existing key adds
;; none, and dissoc removes one.
(def m {:a 1 :b 2 :c 3})
(println (count {})
         (count {:a 1 :b 2})
         (count m)
         (count (assoc m :a 9))
         (count (dissoc m :a))
         (count {:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8 :i 9 :j 10}))
