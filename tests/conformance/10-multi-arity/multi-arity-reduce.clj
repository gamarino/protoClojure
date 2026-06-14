;; EXPECT: 15 106
(defn my-reduce
  ([f coll] (my-reduce f (first coll) (rest coll)))
  ([f acc coll]
   (if (empty? coll)
     acc
     (recur f (f acc (first coll)) (rest coll)))))
(println (my-reduce + (list 1 2 3 4 5))
         (my-reduce + 100 (list 1 2 3)))
