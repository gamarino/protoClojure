;; EXPECT: 1 2 3 4 5 true
;; Map destructuring: {local key}, :keys, :strs, :syms, :or and :as.
(let [{x :x :keys [y z] :strs [s] :syms [q] :or {z 9} :as m}
      {:x 1 :y 2 "s" 4 'q 5}]
  (println x y (- z 6) s q (= m {:x 1 :y 2 "s" 4 'q 5})))
