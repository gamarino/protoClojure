;; Build a 10_000-element list via cons, then reduce + over it.
;; Result: 49995000.
(defn build-list [n]
  (loop [i 0 acc (list)]
    (if (>= i n)
      acc
      (recur (+ i 1) (cons i acc)))))
(println (reduce + (build-list 10000)))
