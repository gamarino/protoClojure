;; zerodiv: native division by zero (/ 1 z), caught as ArithmeticException, 200000 times.
(defn run [n z] (loop [i 0 c 0] (if (< i n) (recur (inc i) (+ c (try (/ 1 z) (catch ArithmeticException e 1)))) c)))
(println "CHECK" (run 200000 0))
