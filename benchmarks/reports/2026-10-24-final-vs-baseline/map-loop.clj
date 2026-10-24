;; Keyword-keyed map reads and assoc in a loop (exercises keyIsIdentity,
;; which now asks whether an object key is a set). Prints the work done.
(defn run [n]
  (loop [i 0 m {:a 1 :b 2 :c 3} acc 0]
    (if (< i n)
      (recur (+ i 1) (assoc m :a i) (+ acc (:a m) (get m :b) (m :c)))
      acc)))
(println "map-loop" (run 2000000))
