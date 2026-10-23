;; EXPECT: 6 (clojure.core/+ 1 2 3)
;; defmacro parameters destructure like fn parameters.
(defmacro add-pair [[a b] & more] `(+ ~a ~b ~@more))
(println (add-pair [1 2] 3) (macroexpand-1 '(add-pair [1 2] 3)))
