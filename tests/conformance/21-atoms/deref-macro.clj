;; EXPECT: 7 7
(def a (atom 7))
(println (deref a) @a)
