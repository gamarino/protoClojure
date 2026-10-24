;; EXPECT: [6 2568 1 -1000000000000000000000000000000 -123456789012345678901234 2 6 -1 -2 5]
;; Clojure's auto-promoting `+'` `-'` `*'` `inc'` `dec'` are the same
;; functions as `+ - * inc dec`, which already promote (D14); `bigint` and
;; `biginteger` answer an exact integer for an integer, a float (truncated)
;; or a string of digits.
(println [(*' 2 3) (count (str (reduce *' (range 1 1001)))) (bigint 1.9) (bigint -1e30)
          (biginteger "-123456789012345678901234") (inc' 1) (+' 1 2 3) (dec' 0) (-' 5 7)
          (bigint 5)])
