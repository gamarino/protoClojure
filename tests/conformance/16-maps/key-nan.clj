;; EXPECT: nil nil :a nil :z
;; A NaN map key is matched only by the identical NaN object: not by 0, and
;; not by another NaN. NaN keys used to be matched by 0, 0.0 and every NaN.
(def nan ##NaN)
(println (get {nan :a} 0) (get {0 :z} nan) (get {nan :a} nan)
         (get {nan :a} (/ 0 0.0)) (get {0 :z} -0.0))
