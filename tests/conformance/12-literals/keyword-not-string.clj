;; EXPECT: false true :kw true
;; A keyword is not a string, but str renders its spelling.
(println (string? :kw) (string? ":kw") (str :kw) (= (str :kw) ":kw"))
