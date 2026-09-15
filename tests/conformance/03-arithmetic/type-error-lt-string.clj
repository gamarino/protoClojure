;; EXPECT-ERROR: ClassCastException: < expects a number, got a string
;; The LT opcode compared a number with a string: (< 1 "a") returned true.
(println (< 1 "a"))
