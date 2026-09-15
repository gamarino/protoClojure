;; EXPECT-ERROR: ClassCastException: < expects a number, got an atom
;; The < primitive (reached through reduce) names the type of the offending value.
(println (reduce < (list 1 (atom 2))))
