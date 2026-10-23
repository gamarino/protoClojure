;; EXPECT-ERROR: compile error: Can't take value of a macro: one
;; A macro is not a value: naming it outside call position is a compile
;; error, as in Clojure.
(defmacro one [x] x)
(def f one)
