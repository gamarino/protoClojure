;; EXPECT: {:y 1, :x 2}
;; The :as map of keyword arguments keeps the order the caller passed them.
(defn f [& {:keys [x y] :as o}] (println o))
(f :y 1 :x 2)
