;; EXPECT: fn* 1 3
;; The reader writes #(...) as Clojure's reader does: (fn* [p1__N#] ...).
(def f '#(inc %))
(println (first f) (count (second f)) (count f))
