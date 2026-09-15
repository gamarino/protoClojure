;; EXPECT: -0.0 -0.0 -0.0 [-0.0] -0.0 0.0
;; Negative zero keeps its sign when printed: the printer tested whether the
;; double equalled its long long cast, which is true for -0.0, and printed the
;; integer 0 followed by ".0".
(println (- 0.0) -0.0 (* -1.0 0.0) [(- 0.0)] (str -0.0) 0.0)
