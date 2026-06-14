;; map (* x x) + reduce + over 0..999. Result: 332833500.
(defn build-list [n]
  (loop [i 0 acc (list)]
    (if (>= i n)
      acc
      (recur (+ i 1) (cons i acc)))))
(println (reduce + (map (fn [x] (* x x)) (build-list 1000))))
