(println (count (for [x (range 300) y (range 300) :when (even? (+ x y))] [x y])))
