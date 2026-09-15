;; EXPECT-ERROR: ClassCastException: <= expects a number, got nil
;; The LE opcode compared nil with a number: (<= nil 1) returned true.
(println (<= nil 1))
