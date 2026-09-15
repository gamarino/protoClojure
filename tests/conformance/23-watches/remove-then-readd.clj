;; EXPECT: true
;; remove-watch drops one watch and leaves the others; a watch added again
;; fires once. Watch order is unspecified, as on the JVM.
(def a (atom 0))
(def hits (atom {}))
(defn record [k r o n] (swap! hits (fn [h] (assoc h k (inc (get h k 0))))))
(add-watch a :a record)
(add-watch a :b record)
(remove-watch a :a)
(add-watch a :a record)
(reset! a 1)
(println (= @hits {:a 1 :b 1}))
