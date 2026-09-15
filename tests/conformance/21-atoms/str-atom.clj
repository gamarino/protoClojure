;; EXPECT: #<atom 1> #<atom {:a 1}> true
;; str renders an atom the way println and the REPL print it.
(def a (atom 1))
(println (str a)
         (str (atom {:a 1}))
         (= (str a) "#<atom 1>"))
