;; EXPECT: 1 12345678901234567890 -7 3 true
;; The N suffix marks an integer literal as a big integer, as in Clojure; the
;; value is an ordinary integer here (promotion is automatic, D14).
(println 1N 12345678901234567890N -7N (+ 1N 2) (= 5N 5))
