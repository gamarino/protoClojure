;; EXPECT: 7 nil true
;; With no rest arguments the rest parameter is nil, as in Clojure, so
;; `(if rest ...)` tells "no more arguments" apart.
(defn show [a & rest] (println a rest (nil? rest)))
(show 7)
