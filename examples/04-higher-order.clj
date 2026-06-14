(defn make-mul [k] (fn [x] (* x k)))
(defn even? [x] (= 0 (- x (* 2 (- x (- x (* 2 (/ x 2)))))))) ;; placeholder

(println (map (make-mul 3) (list 1 2 3 4 5)))
(println (reduce + (map (fn [x] (* x x)) (list 1 2 3 4 5))))
(println (filter (fn [x] (> x 10)) (map (make-mul 4) (list 1 2 3 4 5))))
(println (apply + (filter (fn [x] (> x 3)) (list 1 2 3 4 5 6 7))))
