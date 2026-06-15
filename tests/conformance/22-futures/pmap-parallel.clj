;; EXPECT: (75025 75025 75025 75025)
;; Session 18: pmap actually runs in parallel — wall-clock speedup
;; visible under `time`. Functional output is the same as `map`.
(defn fib [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))
(println (pmap fib (list 25 25 25 25)))
