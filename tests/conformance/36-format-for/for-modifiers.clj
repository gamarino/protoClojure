;; EXPECT: (1 9 25) ([1 0] [2 0] [2 1]) (0 2 4) ([1 2] [1 3])
;; for's :when, :let and :while, in order: :while stops the iteration of the
;; binding before it, not the outer ones.
(println (for [x (range 10) :when (odd? x) :let [y (* x x)] :while (< y 30)] y)
         (for [x (range 3) y (range 3) :while (< y x)] [x y])
         (for [x (range 6) :when (even? x)] x)
         (for [x [1] :let [[a b] [2 3]] y [a b]] [x y]))
