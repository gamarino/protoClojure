;; EXPECT-ERROR: read error: Unknown symbolic value: Infinity
;; Only Inf, -Inf and NaN follow ##, as in JVM Clojure.
(println ##Infinity)
