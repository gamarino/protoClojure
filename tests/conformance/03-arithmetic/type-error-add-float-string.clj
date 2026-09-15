;; EXPECT-ERROR: ClassCastException: + expects a number, got a string
;; The ADD opcode returned the float and ignored the string: (+ 1.5 "a") was 1.5.
(println (+ 1.5 "a"))
