;; EXPECT: {:a 1, :b [1 2]} 16 true
;; str renders a map the way println prints it.
(def m {:a 1 :b [1 2]})
(println (str m)
         (count (str m))
         (= (str {:a 1}) "{:a 1}"))
