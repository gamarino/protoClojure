;; 15-top-words.clj -- the three words a text on standard input uses most
;; (docs/tutorial/14-input-and-output.md, "A script in twenty lines").
(ns top-words
  (:require [clojure.string :as str]))

(defn words [line]
  (->> (str/split (str/lower-case line) #"[^a-z']+")
       (remove str/blank?)))

(defn top [n counts]
  (->> counts
       (sort-by (juxt (comp - val) key))   ; most frequent first, then by word
       (take n)))

(doseq [[word k] (top 3 (frequencies (mapcat words (line-seq *in*))))]
  (println (format "%-6s %d" word k)))
(when (some #(= "--total" %) *command-line-args*)
  (println "done"))
