;; EXPECT: #<future 42>
(def f (future 42))
@f                          ;; force completion
(println f)
