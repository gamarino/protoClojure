;; EXPECT: true true true true ##Inf ##-Inf ##NaN
;; ##Inf and ##-Inf are the IEEE infinities that float division produces,
;; ordered beyond every finite double, and arithmetic on them follows IEEE 754.
(println (= ##Inf (/ 1 0.0)) (= ##-Inf (/ -1 0.0))
         (< 1.0E308 ##Inf) (> -1.0E308 ##-Inf)
         (+ ##Inf 1) (- ##Inf) (+ ##Inf ##-Inf))
