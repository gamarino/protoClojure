;; EXPECT: false true false false false false false false false false false false false
;; Comparisons follow IEEE 754 for NaN: every ordering and `=` with a NaN is
;; false, and not= is true, whether the operator is compiled to an opcode or
;; called as a function. The two NaNs are computed separately. `<=`, `>=` and
;; `=` used to be true, because protoCore's compare reports NaN equal to every
;; number.
(def a ##NaN)
(def b (/ 0 0.0))
(def big 12345678901234567890)
(println (= a b) (not= a b) (< a 1) (> a 1) (<= a 1) (>= a 1)
         (= a 1) (= 1 a) (<= a 1.5) (>= big b) (= a big)
         (apply <= (list a 1)) (= a 0))
