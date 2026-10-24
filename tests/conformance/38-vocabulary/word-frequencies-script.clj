;; EXPECT: the=3 fox=2 brown=1
;; The kind of 20-line script Part C is about: aliases, #(), frequencies,
;; sort-by, take, str/join.
(ns words
  (:require [clojure.string :as str]))

(def text "the quick brown fox jumps over the lazy dog the fox")

(defn word-counts [s]
  (->> (str/split s #"\s+")
       (map str/lower-case)
       (remove str/blank?)
       frequencies))

(defn top [n counts]
  (->> counts
       (sort-by (juxt (comp - val) key))
       (take n)
       (map #(str (key %) "=" (val %)))))

(println (str/join " " (top 3 (word-counts text))))
