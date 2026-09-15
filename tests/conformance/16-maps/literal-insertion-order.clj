;; EXPECT: {:c 3, :a 1, :b 2}
;; A map literal keeps its source order, whatever the key hashes are.
(println {:c 3 :a 1 :b 2})
