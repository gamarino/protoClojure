;; EXPECT: false true true true false false
;; not= is the negation of =.
(println (not= {:a 1 :b 2} {:b 2 :a 1})
         (not= {:a 1} {:a 2})
         (not= {:a 1} [:a 1])
         (not= 1 2)
         (not= "s" "s")
         (not= {:a 1} {:a 1} {:a 1}))
