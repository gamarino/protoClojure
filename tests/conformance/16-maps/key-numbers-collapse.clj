;; EXPECT: 2 :a :b {[1 2] :b} 1
;; Numbers of different types are different keys, as in JVM Clojure, although
;; (= 1 1.0) is true (deviation D15). A vector and a list with equal elements
;; are one key: assoc keeps the stored key object and replaces the value.
(def m (hash-map 1 :a 1.0 :b))
(println (count m)
         (get m 1)
         (get m 1.0)
         (assoc {[1 2] :a} (list 1 2) :b)
         (count (assoc {[1 2] :a} (list 1 2) :b)))
