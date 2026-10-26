;; EXPECT: 4000 true
;; Each thread matches with its own match data and JIT stack (M48): futures
;; matching the same pattern at once all get their own groups.
(def p #"(\d+)-(\w+)")
(defn work [t] (every? (fn [i] (= [(str t "-x" i) (str t) (str "x" i)] (re-find p (str "id " t "-x" i))))
                       (range 1000)))
(def fs (doall (map (fn [t] (future (work t))) (range 4))))
(println (* 4 1000) (every? true? (map deref fs)))
