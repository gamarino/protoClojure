;; EXPECT: {:a 9, :b 2, :c 3}
;; assoc of an existing key replaces the value in place: the key keeps its
;; position.
(println (assoc {:a 1 :b 2 :c 3} :a 9))
