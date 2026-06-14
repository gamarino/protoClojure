;; EXPECT: 11
(def a (atom 10))
(swap! a inc)
(println @a)
