(defn build-list [n]
  (loop [i 0 acc (list)]
    (if (>= i n)
      acc
      (recur (+ i 1) (cons i acc)))))
(println (reduce + (build-list 300000)))
