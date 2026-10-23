;; EXPECT: 7 -1
;; A macro used inside a function: it is expanded when the function is
;; compiled, and the expansion refers to the function's own locals.
(defmacro swap-args [call] (list (first call) (nth call 2) (nth call 1)))
(defn g [x y] (swap-args (- x y)))
(println (g 3 10) (g 1 0))
