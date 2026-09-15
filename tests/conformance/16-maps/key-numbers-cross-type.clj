;; EXPECT: nil nil nil nil nil :half :one :big true false
;; An integer key is not found with an equal float, nor a float key with an
;; equal integer, at any size (2^64), as in JVM Clojure. (= 1 1.0) stays true
;; (deviation D15), but maps whose keys differ only in numeric type are not =.
;; Numbers of the same type computed separately find each other.
(def big (* 4611686018427387904 4))
(println (get {1 :one} 1.0)
         (get {1.0 :one} 1)
         (get {big :big} 18446744073709551616.0)
         (get {18446744073709551616.0 :big} big)
         (get {1 :one} 1.5)
         (get {0.5 :half} (/ 1.0 2))
         (get {1.0 :one} (/ 2.0 2))
         (get {big :big} (* 4294967296 4294967296))
         (= 1 1.0)
         (= {1 :a} {1.0 :a}))
