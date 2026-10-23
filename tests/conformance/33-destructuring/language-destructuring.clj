;; EXPECT: [1 2 (3 4) [1 2 3 4]] [1 2 0 3] 6 :a 1
;; LANGUAGE.md §5.4: destructuring.
(def r1 (let [[a b & more :as all] [1 2 3 4]] [a b more all]))
(def r2 (let [{x :x, :keys [y z], :strs [s], :or {z 0}, :as m} {:x 1 :y 2 "s" 3}] [x y z s]))
(defn area [{:keys [w h] :or {h 1}}] (* w h))
(print r1 r2 (area {:w 3 :h 2}) "")
(doseq [[k v] {:a 1}] (println k v))
