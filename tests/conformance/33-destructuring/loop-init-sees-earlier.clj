;; EXPECT: 3
;; A later loop init sees the names an earlier binding form destructured.
(println (loop [[a b] [1 2] c (+ a b)] c))
