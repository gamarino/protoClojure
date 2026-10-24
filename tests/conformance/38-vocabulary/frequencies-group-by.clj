;; EXPECT: 5 2 [1 3 5] [0 2 4] 2 [a a]
;; frequencies and group-by.
(def f (frequencies (seq "abracadabra")))
(def g (group-by odd? (range 6)))
(println (f "a") (f "b") (get g true) (get g false) (count (frequencies [1 1.0 1]))
         (get (group-by count ["a" "bb" "a"]) 1))
