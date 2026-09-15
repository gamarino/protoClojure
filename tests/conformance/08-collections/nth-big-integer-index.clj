;; EXPECT-ERROR: IndexOutOfBoundsException: nth index 12345678901234567890 is out of bounds (count 3)
;; A big integer index is out of bounds; the error names nth and the index
;; (it was protoCore's "LargeInteger value exceeds long long range.").
(println (nth [1 2 3] 12345678901234567890))
