;; EXPECT: 110
(defn affine [a b]
  (fn [x] (+ (* a x) b)))
(def f (affine 10 20))
(println (f 9))
