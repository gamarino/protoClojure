;; EXPECT: true true true :a 1 false
;; A lazy seq equals a list or vector with the same elements, and is the same
;; map key and set element.
(println (= (map inc [1 2]) [2 3]) (= '(2 3) (map inc [1 2])) (= (range 3) (list 0 1 2))
         (get {(map inc [1]) :a} [2]) (count (hash-set (range 2) [0 1]))
         (= (map inc [1]) [2 3]))
