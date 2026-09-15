;; EXPECT-ERROR: StringIndexOutOfBoundsException: subs begin 0, end 12345678901234567890, length 3
;; A big integer end is out of range as well.
(println (subs "abc" 0 12345678901234567890))
