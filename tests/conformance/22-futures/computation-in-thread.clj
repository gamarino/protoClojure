;; EXPECT: 75025
(defn fib [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))
(println @(future (fib 25)))
