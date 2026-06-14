(defn greet
  ([] (greet "world"))
  ([name] (println "hello," name)))

(greet)
(greet "clojurian")

(defn my-reduce
  ([f coll] (my-reduce f (first coll) (rest coll)))
  ([f acc coll]
   (if (empty? coll)
     acc
     (recur f (f acc (first coll)) (rest coll)))))

(println (my-reduce + (list 1 2 3 4 5)))
(println (my-reduce + 100 (list 1 2 3)))

(println
  (cond
    (= 1 2) :impossible
    (and (> 5 0) (< 5 100)) :in-range
    :else :nope))
