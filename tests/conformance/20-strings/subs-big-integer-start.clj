;; EXPECT-ERROR: StringIndexOutOfBoundsException: subs begin 12345678901234567890, end 3, length 3
;; A big integer start is out of range; the error names subs and the bounds, as
;; Java's message does (it was "LargeInteger value exceeds long long range.").
(println (subs "abc" 12345678901234567890))
