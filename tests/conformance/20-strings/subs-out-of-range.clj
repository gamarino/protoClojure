;; EXPECT-ERROR: StringIndexOutOfBoundsException: subs begin 2, end 1, length 3
;; An end before the start names the bounds (the message was
;; "subs: bounds out of range").
(println (subs "abc" 2 1))
