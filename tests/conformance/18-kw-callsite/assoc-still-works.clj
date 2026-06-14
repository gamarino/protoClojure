;; EXPECT: {:a 1, :b 2, :c 3}
;; assoc is a primitive that expects (m k v k v ...) positionals — the
;; CALL_KW unpack restores that shape.
(println (assoc {:a 1} :b 2 :c 3))
