;; EXPECT-ERROR: ClassCastException: > expects a number, got a map
;; The GT opcode compared a big integer with a map instead of raising an error.
(println (> 9007199254740993 {:a 1}))
