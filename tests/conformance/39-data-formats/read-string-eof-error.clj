;; EXPECT-ERROR: EOF while reading
;; A string with no form is an error for clojure.core/read-string.
(read-string "   ")
