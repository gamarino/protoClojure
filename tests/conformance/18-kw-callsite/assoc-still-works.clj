;; EXPECT: {:a 1, :b 2, :c 3}
;; assoc is a primitive that expects (m k v k v ...) positionals; CALL_KW
;; passes trailing keyword arguments to it unchanged, in source order.
(println (assoc {:a 1} :b 2 :c 3))
