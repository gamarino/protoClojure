;; EXPECT: false false
;; Maps of the same size with different key sets are not =, even when the
;; values match or are nil.
(println (= {:a 1 :b 2} {:a 1 :c 2})
         (= {:a nil} {:b nil}))
