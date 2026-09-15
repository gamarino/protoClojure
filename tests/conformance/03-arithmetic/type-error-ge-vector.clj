;; EXPECT-ERROR: ClassCastException: >= expects a number, got a vector
;; The GE opcode compared a vector with a number: (>= [1] 1) returned true.
(println (>= [1] 1))
