;; EXPECT: nil 7
;; (try) is nil; a try with no clauses is a do.
(println (try) (try 3 5 7))
