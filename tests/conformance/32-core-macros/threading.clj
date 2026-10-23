;; EXPECT: 8 (2 3 4) 6 "ab"
;; -> threads as the first argument, ->> as the last, as-> through a name.
(println (-> 3 inc (* 2)) (->> [1 2 3] (map inc)) (as-> 1 x (+ x 1) (* x 3))
         (pr-str (-> "a" (str "b"))))
