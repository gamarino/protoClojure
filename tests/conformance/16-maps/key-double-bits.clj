;; EXPECT: :x :x nil nil :n :z 2
;; Double keys match by their 64-bit pattern: doubles computed separately
;; find each other, 0.0 and -0.0 are different keys (as in JVM Clojure), and
;; ##NaN finds itself (JVM Clojure never finds a NaN key).
(def d (/ 3.0 2))
(println (get {1.5 :x} d)
         (get {d :x} 1.5)
         (get {0.0 :z} -0.0)
         (get {-0.0 :z} 0.0)
         (get {##NaN :n} ##NaN)
         (get {-0.0 :z} (- 0.0))
         (count (hash-map 0.0 :a -0.0 :b)))
