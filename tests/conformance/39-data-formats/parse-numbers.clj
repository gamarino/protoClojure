;; EXPECT: [12 -9223372036854775808 nil 7 nil nil 1.5 ##-Inf 100000.0 0.5 5.0 nil nil true false nil]
;; Clojure 1.11's parse-long, parse-double and parse-boolean: nil for a
;; string that spells no value (parse-long beyond 64 bits included).
(println [(parse-long "12") (parse-long "-9223372036854775808") (parse-long "9223372036854775808")
          (parse-long "+7") (parse-long "1.0") (parse-long "")
          (parse-double " 1.5 ") (parse-double "-Infinity") (parse-double "1e5") (parse-double ".5")
          (parse-double "5.") (parse-double "x") (parse-double "1e")
          (parse-boolean "true") (parse-boolean "false") (parse-boolean "yes")])
