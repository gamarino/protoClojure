(defn build-list [n]
  (loop [i 0 acc (list)]
    (if (>= i n)
      acc
      (recur (+ i 1) (cons i acc)))))
(println (reduce + (map (fn [x] (* x x)) (build-list 100000))))
