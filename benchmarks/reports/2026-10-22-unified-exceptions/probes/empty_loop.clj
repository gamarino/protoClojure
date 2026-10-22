;; empty_loop: the loop alone, 2000000 iterations.
(defn run [n] (loop [i 0 c 0] (if (< i n) (recur (inc i) (+ c 1)) c)))
(println "CHECK" (run 2000000))
