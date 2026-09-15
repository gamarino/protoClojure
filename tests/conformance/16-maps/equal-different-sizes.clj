;; EXPECT: false false false false
;; A map is never = to a map of a different size.
(println (= {:a 1} {:a 1 :b 2})
         (= {:a 1 :b 2} {:a 1})
         (= {} {:a 1})
         (= {:a nil} {}))
