;; EXPECT-ERROR: ClassCastException: / expects a number, got nil
;; / reported "argument 1 is nil" without the error class.
(println (/ 6 nil))
