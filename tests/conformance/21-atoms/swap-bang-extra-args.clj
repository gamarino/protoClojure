;; EXPECT: 53
(def a (atom 10))
(swap! a + 40 3)
(println @a)
