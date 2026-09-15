;; EXPECT-ERROR: ClassCastException: dec expects a number, got a fn
;; dec named the argument position but not the type of the value.
(println (dec println))
