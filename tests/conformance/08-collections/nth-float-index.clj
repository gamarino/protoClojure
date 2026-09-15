;; EXPECT-ERROR: ClassCastException: nth expects an integer, got a float
;; A non-integer index raises the ClassCastException analogue naming nth and
;; the type (the message was "nth: argument 1 is not an integer").
(println (nth [1 2 3] 1.5))
