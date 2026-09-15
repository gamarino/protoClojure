;; EXPECT-ERROR: ClassCastException: * expects a number, got a string
;; The * primitive named the argument position but not the type of the value.
(println (apply * (list "ab" 3)))
