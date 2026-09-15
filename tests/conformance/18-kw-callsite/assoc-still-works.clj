;; EXPECT: true 1 2 3
;; assoc is a primitive that expects (m k v k v ...) positionals; CALL_KW
;; passes trailing keyword arguments to it unchanged, in source order.
(def m (assoc {:a 1} :b 2 :c 3))
(println (= m {:a 1 :b 2 :c 3}) (:a m) (:b m) (:c m))
