;; EXPECT: :one :one :big :big nil :half
;; Numbers that are = across types (deviation D15) hash equally, so an
;; integer key is found with an equal float and vice versa, including
;; integers beyond the long range (2^64).
(def big (* 4611686018427387904 4))
(println (get {1 :one} 1.0)
         (get {1.0 :one} 1)
         (get {big :big} 18446744073709551616.0)
         (get {18446744073709551616.0 :big} big)
         (get {1 :one} 1.5)
         (get {0.5 :half} (/ 1.0 2)))
