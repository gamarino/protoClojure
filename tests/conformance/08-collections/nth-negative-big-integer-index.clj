;; EXPECT-ERROR: IndexOutOfBoundsException: nth index -99999999999999999999 is out of bounds (count 2)
;; A negative big integer index on a list is out of bounds as well.
(println (nth (list 1 2) -99999999999999999999))
