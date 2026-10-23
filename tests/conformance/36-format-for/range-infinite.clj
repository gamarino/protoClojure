;; EXPECT-ERROR: (range) with no end needs lazy sequences
;; (range) with no end is infinite, which needs lazy sequences.
(first (range))
