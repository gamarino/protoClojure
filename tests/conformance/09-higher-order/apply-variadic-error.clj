;; EXPECT-ERROR: apply: only the two-argument form
;; The variadic form (apply f x coll) is not implemented; the error says so.
(apply + 1 [2])
