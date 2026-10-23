;; EXPECT: :two (if a (do b))
;; LANGUAGE.md §6.2: the clojure.core macros.
(defn total [items] (->> items (map :n) (reduce +)))
(when-let [t (total [{:n 1} {:n 3}])]
  (println "total" t))                     ;; prints total 4
(println (case (count "ab") 1 :one 2 :two :many) (macroexpand '(when a b)))
