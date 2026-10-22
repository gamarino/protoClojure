;; try_no_throw: enter a try whose body does not throw, 2000000 times.
(defn run [n] (loop [i 0 c 0] (if (< i n) (recur (inc i) (+ c (try 1 (catch Exception e 0)))) c)))
(println "CHECK" (run 2000000))
