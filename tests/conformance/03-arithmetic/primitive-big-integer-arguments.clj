;; EXPECT: 12345678901234567891 true true 1234567890123456789 12345678901234567891 9223372036854775808 -3 3.5
;; The arithmetic and comparison primitives accept big integers and compare
;; integers exactly, beyond the 2^53 range of a double.
(println (apply + (list 12345678901234567890 1))
         (apply < (list 9007199254740992 9007199254740993 9007199254740994))
         (< 1 12345678901234567890 12345678901234567891)
         (apply / (list 12345678901234567890 10))
         (inc 12345678901234567890)
         (apply / (list -9223372036854775808 -1))
         (/ 7 -2)
         (/ 7 2.0))
