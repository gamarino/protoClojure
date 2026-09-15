;; EXPECT-ERROR: IndexOutOfBoundsException: nth index 3 is out of bounds (count 3)
;; An index past the end names the index and the count (the message was
;; "nth: index out of bounds").
(println (nth [1 2 3] 3))
