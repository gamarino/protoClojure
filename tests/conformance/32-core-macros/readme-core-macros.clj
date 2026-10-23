;; EXPECT: 20 30 :two
;; README.md, "The clojure.core macros": threading, when-let, doseq, case.
(defn total [items] (->> items (map :n) (reduce +)))
(when-let [t (total [{:n 1} {:n 3}])]
  (println "total" t))                           ;; => total 4
(doseq [x [1 2]] (print (-> x inc (* 10)) ""))  ;; => 20 30
(println (case (count "ab") 1 :one 2 :two :many)) ;; => :two
