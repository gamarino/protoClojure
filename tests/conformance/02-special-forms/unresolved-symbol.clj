;; EXPECT-ERROR: unable to resolve symbol: never-defined
;; A symbol that was never defined is still an error.
(def x nil)
(println x never-defined)
