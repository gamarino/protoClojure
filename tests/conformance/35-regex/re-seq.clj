;; EXPECT: (1 22 333) nil ([a1 a 1] [b2 b 2]) 3
;; re-seq answers every successive match, nil when there is none.
(println (re-seq #"\d+" "1 22 333") (re-seq #"z" "abc") (re-seq #"(\w)(\d)" "a1 b2")
         (count (re-seq #"\w+" "the quick fox")))
