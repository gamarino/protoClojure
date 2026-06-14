;; EXPECT: :a 1 :b 2
;; Primitive println is not kw-based; the VM unpacks the synthetic
;; kwMap back into positional :k v pairs, preserving session-13 semantics.
(println :a 1 :b 2)
