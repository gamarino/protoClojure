;; EXPECT: () true 0 (1)
;; `()` evaluates to the empty list, as in Clojure.
(println () (= () (list)) (count ()) (conj () 1))
