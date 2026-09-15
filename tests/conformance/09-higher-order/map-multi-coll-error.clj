;; EXPECT-ERROR: map: only (map f coll) is supported
;; Multi-collection map is not implemented; the error says so.
(println (map + [1] [2]))
