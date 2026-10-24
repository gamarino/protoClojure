;; EXPECT: (:x :x :x) (:y :y) (1 2 3 1 2) () (7 7)
;; repeat and cycle, endless or bounded; repeatedly calls a fn.
(println (take 3 (repeat :x)) (repeat 2 :y) (take 5 (cycle [1 2 3])) (cycle [])
         (repeatedly 2 (fn [] 7)))
