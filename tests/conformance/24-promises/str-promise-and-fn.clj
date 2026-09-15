;; EXPECT: #<promise pending> #<promise 5> #<fn>
;; str renders promises and fns the way println prints them.
(def p (promise))
(def before (str p))
(deliver p 5)
(println before (str p) (str (fn [x] x)))
