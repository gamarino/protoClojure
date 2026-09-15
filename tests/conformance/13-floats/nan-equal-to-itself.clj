;; EXPECT: true false true
;; A NaN object is `=` to itself (the identity test comes first, as in JVM
;; Clojure's Util.equiv), but two NaN values read or computed separately are
;; not `=`.
(def x ##NaN)
(println (= x x) (= ##NaN ##NaN) (= [x] [x]))
