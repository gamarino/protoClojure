;; EXPECT: true :x true false
;; A big integer literal is = to the same value computed at run time, and
;; hashes consistently with =, so it finds the map entry keyed by that value.
(println (= 12345678901234567890 (+ 12345678901234567889 1))
         (get {12345678901234567890 :x} (+ 12345678901234567889 1))
         (contains? (hash-map (* 4294967296 4294967296) :y) 18446744073709551616)
         (= 12345678901234567890 12345678901234567891))
