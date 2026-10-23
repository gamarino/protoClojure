;; EXPECT-ERROR: compile error: StackOverflowError
;; A macro that always expands into another call of itself ends in
;; StackOverflowError, reported as a compile error.
(defmacro forever [] (list 'forever))
(forever)
