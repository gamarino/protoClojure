;; EXPECT: true 3
;; assoc takes several key/value pairs. Map print order is unspecified, so the
;; result is checked by value.
(def m (assoc {:a 1} :b 2 :c 3))
(println (= m {:a 1 :b 2 :c 3}) (count (keys m)))
