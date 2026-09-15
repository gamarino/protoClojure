;; EXPECT: true true true false
;; The empty vector and the empty list are =, including empty sequences
;; produced by rest.
(println (= [] (list))
         (= (list) [])
         (= (rest [1]) [])
         (= [] (list nil)))
