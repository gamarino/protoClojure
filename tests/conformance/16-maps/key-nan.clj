;; EXPECT: :a nil nil nil :z nil
;; A NaN key is matched by its exact bit pattern: ##NaN finds itself (JVM
;; Clojure never finds a NaN key), and no number finds it. A NaN produced by
;; arithmetic can have another bit pattern (0.0/0.0 sets the sign bit on
;; x86-64) and then does not find a ##NaN key; that case depends on the
;; processor and is covered by the unit tests. 0 and -0.0 are different keys.
(def nan ##NaN)
(println (get {nan :a} ##NaN)
         (get {nan :a} 0)
         (get {0 :z} nan)
         (get {0.0 :z} nan)
         (get {0 :z} 0)
         (get {0 :z} -0.0))
