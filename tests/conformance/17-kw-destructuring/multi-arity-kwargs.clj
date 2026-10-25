;; EXPECT: :none [1 nil 9] [1 2 9] [1 2 3] [1 5] [1 5 9] [1 5 6] [1 7 9] ([1 2 9] [2 2 9]) ([1 5 9] [2 nil 6])
;; A multi-arity fn whose arity takes keyword arguments (`[a & {:keys ...}]`)
;; is called with key/value pairs, directly, through `apply` and from a
;; primitive (map); the fixed arity and the trailing-map form keep working.
;; Every value checked against Babashka 1.12.200.
(defn f ([] :none) ([a & {:keys [x y] :or {y 9}}] [a x y]))
(defn g [a & {:keys [x]}] [a x])
(println (f) (f 1) (f 1 :x 2) (f 1 :x 2 :y 3) (apply g [1 :x 5]) (apply f [1 :x 5])
         (apply f 1 [:x 5 :y 6]) (f 1 {:x 7}) (map (fn [a] (f a :x 2)) [1 2])
         (map f [1 2] [:x :y] [5 6]))
