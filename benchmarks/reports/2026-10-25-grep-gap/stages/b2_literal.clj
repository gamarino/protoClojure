;; Stage b2: re-find with a pure literal pattern (engine fixed cost + scan).
(require '[clojure.java.io :as io])
(def lit #"ERROR")
(println "matches" (reduce (fn [n line] (if (re-find lit line) (inc n) n))
                           0 (line-seq (io/reader *in*))))
