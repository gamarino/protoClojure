;; EXPECT: nil 7 nil nil nil
;; A keyword called on nil or on a value that is not a map returns nil, or the
;; not-found argument, as get does.
(println (:a nil)
         (:a nil 7)
         (:a 42)
         (:a "abc")
         (:a [1 2]))
