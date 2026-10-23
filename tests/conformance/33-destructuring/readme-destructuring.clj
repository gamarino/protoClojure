;; EXPECT: ana: clj and 2 more | :a 1
;; README.md, "A flavour of the language": destructuring (the two printed
;; lines are joined on one line here, so the runner checks both).
(defn describe [{:keys [name langs] :or {langs []}}]
  (let [[first-lang & others] langs]
    (str name ": " first-lang " and " (count others) " more")))
(print (describe {:name "ana" :langs ["clj" "py" "st"]}) "| ")
(doseq [[k v] {:a 1}] (println k v))
