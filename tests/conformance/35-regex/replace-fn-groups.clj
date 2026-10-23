;; EXPECT: 2=a 4=b
;; A fn replacement of a pattern with groups receives the match vector.
(println (replace "a2 b4" #"(\w)(\d)" (fn [[_ l d]] (str d "=" l))))
