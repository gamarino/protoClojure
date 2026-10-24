;; EXPECT: false true false true
;; realized? tells whether a lazy seq's body has run.
(def s (map inc [1 2]))
(def r1 (realized? s))
(first s)
(def l (lazy-seq [1]))
(def r2 (realized? l))
(seq l)
(println r1 (realized? s) r2 (realized? l))
