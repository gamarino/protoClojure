;; EXPECT: 3 -3 1 -1 1 -1 -1 1 3333333333333333333333 1 1.5 2.0 2.0
;; quot truncates toward zero, rem takes the dividend's sign, mod the
;; divisor's; exact for integers of any size, IEEE for floats.
(println (quot 7 2) (quot -7 2) (rem 7 2) (rem -7 2) (mod -7 2) (mod 7 -2) (rem -7 -2) (mod 13 4)
         (quot 10000000000000000000000 3) (rem 10000000000000000000000 3)
         (mod 5.5 2) (mod -0.5 2.5) (quot 5.5 2))
