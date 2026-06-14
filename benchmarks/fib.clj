;; fib(30) = 832040. Pure recursion, no allocations.
(defn fib [n]
  (if (< n 2)
    n
    (+ (fib (- n 1)) (fib (- n 2)))))
(println (fib 30))
