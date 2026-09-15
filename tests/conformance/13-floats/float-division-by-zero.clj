;; EXPECT: ##Inf ##-Inf ##NaN ##Inf ##-Inf ##NaN ##Inf ##Inf ##-Inf
;; Division with a float operand follows IEEE 754, as in JVM Clojure: a zero
;; divisor gives an infinity (signed by both operands) or NaN for 0/0. It used
;; to raise "/: divide by zero".
(println (/ 1 0.0) (/ -1 0.0) (/ 0 0.0) (/ 1.0 0) (/ 1 -0.0) (/ 0.0 0.0) (/ 1 2 0.0) (/ 0.0) (/ -0.0))
