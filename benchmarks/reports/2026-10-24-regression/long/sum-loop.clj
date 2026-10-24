(defn sum-to [n]
  (loop [i 0 acc 0]
    (if (> i n)
      acc
      (recur (+ i 1) (+ acc i)))))
(println (sum-to 10000000))
