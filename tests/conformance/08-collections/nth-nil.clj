;; EXPECT: nil nil :d :d
;; nth on nil returns nil, or the not-found value, for any integer index, as
;; in JVM Clojure. (nth nil 0) used to raise "nth: nil collection".
(println (nth nil 0) (nth nil 7) (nth nil 0 :d) (nth nil -1 :d))
