;; EXPECT: 3 true
;; :keys destructuring and keyword call-site pairs with non-ASCII spellings.
(defn f [& {:keys [año] :as opts}] (list año opts))
(def r (f :año 3 :mañana 4))
(println (first r) (= (first (rest r)) {:año 3 :mañana 4}))
