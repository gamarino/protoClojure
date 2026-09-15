;; EXPECT: [1 1.0 "a" :a a 1 1.0 "a" :a a] true false false
;; Equal literals share one constant-pool entry only when they have the same
;; kind: 1 and 1.0, the string "a", the keyword :a and the symbol a stay
;; distinct values.
(def v [1 1.0 "a" :a (quote a) 1 1.0 "a" :a (quote a)])
(println (str v) (string? (nth v 7)) (string? (nth v 8)) (= (nth v 2) (nth v 3)))
