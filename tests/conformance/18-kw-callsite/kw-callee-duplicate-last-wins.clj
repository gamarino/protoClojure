;; EXPECT: 2
;; A keyword-argument fn given the same key twice sees the last value, as in
;; Clojure.
(defn f [& {:keys [a]}] (println a))
(f :a 1 :a 2)
