;; EXPECT-ERROR: ClassCastException: * expects a number, got a string
;; The MUL opcode repeated a string: (* "ab" 3) returned "ababab".
(println (* "ab" 3))
