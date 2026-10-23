;; EXPECT-ERROR: Wrong number of args (2) passed to macro: one
;; A macro called with the wrong number of arguments is a compile error that
;; counts the arguments as written (not the hidden &form and &env).
(defmacro one [x] x)
(one 1 2)
