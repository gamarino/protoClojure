;; EXPECT: (2 3 4 5 6)
;; v0.17 pmap is sequential-but-correct; real per-element parallelism
;; lands in a later session. The surface matches Clojure's though, so
;; user code compiled today picks up the parallel implementation
;; automatically when it ships.
(println (pmap inc (list 1 2 3 4 5)))
