;; EXPECT-ERROR: malformed number literal: 1.5N
;; The N suffix applies to integer literals only.
(println 1.5N)
