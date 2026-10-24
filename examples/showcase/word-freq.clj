;; Word frequencies: the 20 most frequent words of a text read from standard
;; input (ties broken alphabetically), then the number of distinct words and
;; the total. Runs unchanged under protoclj and bb.
;;
;;   protoclj word-freq.clj < corpus.txt
;;   bb word-freq.clj < corpus.txt
(require '[clojure.java.io :as io] '[clojure.string :as str])

(def freqs
  (->> (line-seq (io/reader *in*))
       (mapcat #(re-seq #"[a-z']+" (str/lower-case %)))
       frequencies))

(doseq [[word n] (take 20 (sort-by (juxt (comp - val) key) freqs))]
  (println n word))
(println "distinct" (count freqs) "total" (reduce + (vals freqs)))
