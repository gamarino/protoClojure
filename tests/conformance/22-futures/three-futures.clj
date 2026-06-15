;; EXPECT: 100 400 900
(defn sq [x] (* x x))
(println @(future (sq 10))
         @(future (sq 20))
         @(future (sq 30)))
