;; EXPECT: 5050
(defn sum-to [n]
  (loop [i 0 acc 0]
    (if (< n i)
      acc
      (recur (inc i) (+ acc i)))))
(println (sum-to 100))
