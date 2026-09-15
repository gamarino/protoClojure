;; EXPECT-ERROR: ClassCastException: - expects a number, got a keyword
;; The SUB opcode reported protoCore's "Objects are not integer types for subtraction."
(println (- 5 :k))
