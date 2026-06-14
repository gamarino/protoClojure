;; loop/recur over 1..1_000_000 — should be near-zero allocation. Result 500000500000.
(defn sum-to [n]
  (loop [i 0 acc 0]
    (if (> i n)
      acc
      (recur (+ i 1) (+ acc i)))))
(println (sum-to 1000000))
