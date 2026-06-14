;; factorial(100) — exercises LargeInteger arithmetic.
;; Babashka 1.4 fails on factorial(21) with `long overflow`; protoClojure
;; promotes to LargeInteger automatically and prints the full 158-digit
;; result.
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1) acc (recur (- n 1) (* acc n)))))
(println (factorial 100))
