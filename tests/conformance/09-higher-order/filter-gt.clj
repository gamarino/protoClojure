;; EXPECT: (3 4 5)
(println (filter (fn [x] (> x 2)) (list 1 2 3 4 5)))
