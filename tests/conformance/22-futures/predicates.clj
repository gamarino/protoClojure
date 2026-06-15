;; EXPECT: true false false
(def f (future 1))
@f                       ;; force completion so the worker exits before main does
(println (future? f) (future? 42) (future? "x"))
