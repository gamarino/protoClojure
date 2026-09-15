;; EXPECT: :x :x :y :y nil
;; Vectors and lists used as map keys are hashed by value with one
;; algorithm, so a vector key is found with an equal list and vice versa.
(def m {[1 2] :x (list 3 4) :y})
(println (get m [1 2])
         (get m (list 1 2))
         (get m [3 4])
         (get m (rest [0 3 4]))
         (get m [2 1]))
