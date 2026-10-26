;; Stage b3: prefilter with clojure.string/includes? before the regex.
(require '[clojure.java.io :as io] '[clojure.string :as str])
(def slow-error #"ERROR .* latency=(\d{4,})ms")
(println "matches" (reduce (fn [n line] (if (and (str/includes? line "ERROR")
                                                 (re-find slow-error line)) (inc n) n))
                           0 (line-seq (io/reader *in*))))
