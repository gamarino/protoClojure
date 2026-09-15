;; EXPECT: true true true true
;; Map equality ignores insertion order: two maps are = when they hold the
;; same keys mapped to = values.
(println (= {:a 1 :b 2} {:b 2 :a 1})
         (= {} {})
         (= (assoc (hash-map :c 3) :b 2 :a 1) {:a 1 :b 2 :c 3})
         (= {:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8 :i 9 :j 10}
            {:j 10 :i 9 :h 8 :g 7 :f 6 :e 5 :d 4 :c 3 :b 2 :a 1}))
