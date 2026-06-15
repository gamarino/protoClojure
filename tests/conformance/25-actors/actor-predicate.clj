;; EXPECT: true false false
(def a (actor 1))
(println (actor? a) (actor? 42) (actor? "x"))
