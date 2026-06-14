;; EXPECT: 42
(def a (atom 0))
(reset! a 42)
(println @a)
