;; EXPECT-ERROR: ClassCastException: disj expects a set, got a vector
;; disj takes a set.
(disj [1 2] 1)
