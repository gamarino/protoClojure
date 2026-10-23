;; EXPECT: 4 [0 1] [1 0]
;; cond-> threads through the forms whose test is true; cond->> as the last
;; argument.
(println (cond-> 1 true inc false (* 10) true (* 2))
         (cond-> [] true (conj 0) true (conj 1))
         (vec (cond->> [0 1] true reverse)))
