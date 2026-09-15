;; EXPECT-ERROR: ClassCastException: + expects a number, got nil
;; The ADD opcode reported protoCore's "Objects are not integer types for addition."
(println (+ 1 nil))
