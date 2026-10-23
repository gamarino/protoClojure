;; EXPECT: x=3 y=2.50 nil
;; printf prints (format ...) with no newline and answers nil.
(def r (printf "x=%d y=%.2f " 3 2.5))
(println r)
