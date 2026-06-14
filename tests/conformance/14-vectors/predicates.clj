;; EXPECT: true false false true
(println (vector? [1 2 3])
         (vector? (list 1 2 3))
         (list?   [1 2 3])
         (list?   (list 1 2 3)))
