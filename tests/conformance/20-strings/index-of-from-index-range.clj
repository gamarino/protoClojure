;; EXPECT: 1 nil 1 3
;; index-of follows Java's String.indexOf for the start position: a negative
;; start searches from 0 and a start past the end finds nothing, at any
;; magnitude (-5 returned nil and a big integer start raised an error).
(println (index-of "abcb" "b" -5) (index-of "abcb" "b" 12345678901234567890) (index-of "abcb" "b" -12345678901234567890) (index-of "abcb" "b" 2))
