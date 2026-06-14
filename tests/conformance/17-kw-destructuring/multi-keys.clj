;; EXPECT: 10 20 nil
(defn show [& {:keys [x y z]}]
  (println x y z))
(show {:x 10 :y 20})
