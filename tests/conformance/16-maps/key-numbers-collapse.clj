;; EXPECT: {1 :b} 1 {[1 2] :b}
;; Keys that are = are one key: 1 and 1.0 (deviation D15), and a vector and
;; a list with equal elements. The first key object and position are kept
;; and the last value wins.
(println (hash-map 1 :a 1.0 :b)
         (count (keys (hash-map 1 :a 1.0 :b)))
         (assoc {[1 2] :a} (list 1 2) :b))
