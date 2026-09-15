;; EXPECT: false false true
;; Same keys with a different value are not =; replacing a value with assoc
;; yields a map equal to one built directly.
(println (= {:a 1 :b 2} {:b 3 :a 1})
         (= {"x" "one"} {"x" "two"})
         (= (assoc {:a 1 :b 2} :a 5) {:b 2 :a 5}))
