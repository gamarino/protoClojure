;; EXPECT: 9 3 true
;; assoc of an existing key replaces its value and adds no entry.
(def m (assoc {:a 1 :b 2 :c 3} :a 9))
(println (:a m) (count (keys m)) (= m {:a 9 :b 2 :c 3}))
