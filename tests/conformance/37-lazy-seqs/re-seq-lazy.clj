;; EXPECT: ("1" "2") [a=1 a 1] ("" "" "") x
;; re-seq is lazy: the first match is found at once, the next ones as the seq
;; is read.
(println (pr-str (take 2 (re-seq #"\d" "1a2b3"))) (first (re-seq #"(\w)=(\d)" "a=1 b=2"))
         (pr-str (re-seq #"" "ab")) (first (re-seq #"x" (apply str (repeat 100000 "x")))))
