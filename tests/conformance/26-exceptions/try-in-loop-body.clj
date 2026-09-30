;; EXPECT: 3
;; A try inside a loop body, with recur outside the try, is allowed.
(defn count-failures [n]
  (loop [i 0 failures 0]
    (if (< i n)
      (recur (+ i 1)
             (+ failures (try (if (or (= i 1) (= i 3) (= i 5)) (/ 1 0) 0)
                              (catch ArithmeticException e 1))))
      failures)))
(println (count-failures 6))
