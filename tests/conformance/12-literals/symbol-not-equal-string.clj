;; EXPECT: false false true false false abc
;; A quoted symbol is never = to the string of its name or to the keyword
;; with that name, and is not a string; it prints as its name.
(println (= (quote abc) "abc")
         (= "abc" (quote abc))
         (= (quote abc) (quote abc))
         (= (quote abc) :abc)
         (string? (quote abc))
         (quote abc))
