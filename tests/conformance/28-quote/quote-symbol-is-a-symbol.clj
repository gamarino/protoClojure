;; EXPECT: true false true false true
;; A quoted symbol is a symbol value, distinct from the string of its name;
;; a quoted keyword is the keyword.
(println (symbol? 'foo) (string? 'foo) (keyword? ':k) (= 'foo "foo") (= 'foo (first '(foo))))
