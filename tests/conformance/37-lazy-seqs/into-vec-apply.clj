;; EXPECT: [1 2 3] [1 3 5] #{2} 5050 a1b2c3
;; Functions that need the whole collection realise a lazy seq.
(println (into [] (map inc (range 3))) (vec (filter odd? (range 6))) (set (map inc [1 1]))
         (apply + (range 101)) (apply str (interleave "abc" "123")))
