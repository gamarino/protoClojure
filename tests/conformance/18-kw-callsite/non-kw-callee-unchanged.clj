;; EXPECT: :a 1 :b 2
;; Primitive println is not kw-based; CALL_KW passes the trailing :k v
;; arguments to it unchanged, as positionals in source order.
(println :a 1 :b 2)
