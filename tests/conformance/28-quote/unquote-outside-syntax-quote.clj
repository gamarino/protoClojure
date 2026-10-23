;; EXPECT-ERROR: unquote (~) used outside a syntax-quote (`)
(def x 1)
(println ~x)
