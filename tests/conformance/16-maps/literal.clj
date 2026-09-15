;; EXPECT: {:a 1} true true
;; A map literal prints as a map. With more than one entry the print order is
;; unspecified: the printed form is one of the orderings, and the entries
;; compare by value.
(def m {:a 1 :b 2})
(println {:a 1}
         (or (= (str m) "{:a 1, :b 2}") (= (str m) "{:b 2, :a 1}"))
         (= m (hash-map :b 2 :a 1)))
