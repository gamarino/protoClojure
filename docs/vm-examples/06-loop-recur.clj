(defn sum-to [n]
  (loop [i 0 acc 0]
    (if (> i n)
      acc
      (recur (inc i) (+ acc i)))))

(sum-to 10)
