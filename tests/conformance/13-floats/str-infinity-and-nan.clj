;; EXPECT: Infinity -Infinity NaN [##Inf] 1.0E21
;; str of a bare infinity or NaN gives Double.toString's spelling, as in JVM
;; Clojure; inside a collection the printer's ##Inf form is used.
(def inf (* 1e308 10))
(println (str inf) (str (- inf)) (str (- inf inf)) (str [inf]) (str 1e21))
