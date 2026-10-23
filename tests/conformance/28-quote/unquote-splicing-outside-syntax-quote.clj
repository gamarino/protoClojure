;; EXPECT-ERROR: unquote-splicing (~@) used outside a syntax-quote (`)
(def x [1])
(println (list ~@x))
