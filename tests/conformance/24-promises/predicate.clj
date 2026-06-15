;; EXPECT: true false false
(def p (promise))
(deliver p 1)
(println (promise? p) (promise? 42) (promise? (atom 1)))
