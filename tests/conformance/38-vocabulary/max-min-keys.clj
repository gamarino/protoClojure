;; EXPECT: 3 1 2.5 ccc a [3 1]
;; max, min, max-key and min-key.
(println (max 1 3 2) (min 3 1 2) (max 1 2.5) (max-key count "a" "ccc" "bb") (min-key count "a" "ccc")
         (apply max-key second [[1 1] [2 0] [3 1]]))
