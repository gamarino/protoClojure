;; EXPECT: true 16 true
;; str renders a map the way println prints it. The entry order of a map with
;; more than one entry is unspecified, so either ordering is accepted.
(def m {:a 1 :b [1 2]})
(println (or (= (str m) "{:a 1, :b [1 2]}") (= (str m) "{:b [1 2], :a 1}"))
         (count (str m))
         (= (str {:a 1}) "{:a 1}"))
