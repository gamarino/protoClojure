;; EXPECT: true true true true false true #{}
;; clojure.set's union, intersection, difference, subset? and superset?, in
;; the global namespace (D28).
(println (= (union #{1 2} #{3} #{4}) #{1 2 3 4})
         (= (intersection #{1 2 3} #{2 3 4} #{3 9}) #{3})
         (= (difference #{1 2 3} #{2} #{3}) #{1})
         (subset? #{1} #{1 2})
         (superset? #{1} #{1 2})
         (superset? #{1 2} #{1})
         (union))
