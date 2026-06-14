;; EXPECT: 55
(defn sum-down-from [start]
  (fn []
    (loop [i start acc 0]
      (if (<= i 0)
        acc
        (recur (- i 1) (+ acc i))))))
(def s10 (sum-down-from 10))
(println (s10))
