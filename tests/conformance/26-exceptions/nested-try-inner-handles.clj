;; EXPECT: [:inner 10]
(println (try
           [(try (/ 1 0) (catch ArithmeticException e :inner))
            (+ 4 6)]
           (catch Exception e :outer)))
