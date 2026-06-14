;; EXPECT: true false false
(def a (atom 0))
(println (atom? a) (atom? 42) (atom? "string"))
