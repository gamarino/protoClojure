;; EXPECT: (max 1 2 10)
;; README.md, "A flavour of the language": code is data.
(def form '(+ 1 2 3))
(println (first form) (rest form))
(def xs [1 2])
(println `(max ~@xs 10))
