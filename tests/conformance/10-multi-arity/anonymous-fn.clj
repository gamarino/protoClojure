;; EXPECT: 70 12
(def f (fn
         ([x] (* x 10))
         ([x y] (* x y))))
(println (f 7) (f 3 4))
