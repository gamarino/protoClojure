;; EXPECT: (1 2 3) true 3
;; A quoted datum in a function body is the same value on every call.
(defn data [] '(1 2 3))
(println (data) (= (data) (data)) (count (data)))
