;; EXPECT: 3
;; A try whose body raises nothing yields the body's value; the catch clause
;; never runs.
(println (try (+ 1 2) (catch Exception e :caught)))
