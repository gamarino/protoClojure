;; EXPECT: ##Inf ##-Inf ##NaN [##Inf ##-Inf ##NaN]
;; Infinities and NaN print as JVM Clojure prints them, readably and with
;; println alike (they printed as the C library spelling inf, -inf, -nan).
(def inf (* 1e308 10))
(println inf (- inf) (- inf inf) [inf (- inf) (- inf inf)])
