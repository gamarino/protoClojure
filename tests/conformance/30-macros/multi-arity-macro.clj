;; EXPECT: 0 5 2
;; A macro may have several arities, chosen by the number of arguments.
(defmacro multi ([] 0) ([x] x) ([x & more] (count more)))
(println (multi) (multi 5) (multi 1 2 3))
