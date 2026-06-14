;; EXPECT: 15 (3 4 5)
(println (reduce + [1 2 3 4 5])
         (filter (fn [x] (> x 2)) [1 2 3 4 5]))
