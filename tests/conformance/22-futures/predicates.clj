;; EXPECT: true false false
(println (future? (future 1)) (future? 42) (future? "x"))
