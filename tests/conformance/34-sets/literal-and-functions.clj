;; EXPECT: 3 true false 3 nil 1 true
;; A set literal; count, contains?, a set called as a function of its
;; elements, get, set?.
(def s #{1 2 3})
(println (count s) (contains? s 2) (contains? s 9) (s 3) (s 9) (get s 1) (set? s))
