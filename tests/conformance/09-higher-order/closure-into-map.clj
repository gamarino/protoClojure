;; EXPECT: (3 6 9 12)
(defn make-mul [k] (fn [x] (* x k)))
(println (map (make-mul 3) (list 1 2 3 4)))
