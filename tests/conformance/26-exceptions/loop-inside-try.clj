;; EXPECT: 45
;; A loop wholly inside a try body may recur to its own head.
(defn sum-to [n]
  (try (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc i)) acc))
       (catch Exception e :failed)))
(println (sum-to 10))
