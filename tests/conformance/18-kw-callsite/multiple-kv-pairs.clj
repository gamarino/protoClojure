;; EXPECT: 1 10 20
(defn f [a & {:keys [x y]}]
  (println a x y))
(f 1 :x 10 :y 20)
