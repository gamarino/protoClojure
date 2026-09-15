;; EXPECT-ERROR: ClassCastException: inc expects a number, got a keyword
;; inc named the argument position but not the type of the value.
(println (inc :a))
