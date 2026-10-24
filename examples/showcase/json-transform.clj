;; JSON transform: read an array of order records, keep the active ones with
;; a score of 50 or more, group them by country, and write one JSON row per
;; country: [country orders total-cents average-cents top-user]. Rows are
;; arrays, not objects, so the output does not depend on map key order
;; (which Clojure leaves unspecified). Runs unchanged under protoclj and bb.
;;
;;   protoclj json-transform.clj orders.json
;;   bb json-transform.clj orders.json
(require '[cheshire.core :as json])

(def orders (json/parse-string (slurp (first *command-line-args*)) true))

(defn summary [[country rs]]
  (let [total (reduce + (map :amount_cents rs))
        top   (first (sort-by (juxt (comp - :amount_cents) :id) rs))]
    [country (count rs) total (quot total (count rs)) (:user top)]))

(def rows (->> orders
               (filter #(and (:active %) (>= (:score %) 50)))
               (group-by :country)
               (map summary)
               (sort-by first)))

(println "orders" (count orders) "kept" (reduce + (map second rows)))
(println (json/generate-string (vec rows)))
