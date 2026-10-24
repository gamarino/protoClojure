;; EXPECT: a:3 b:2 c:1|{:stock {:apples 5}} 2 3
;; README.md, "A flavour of the language": the everyday scripting vocabulary.
(ns words (:require [clojure.string :as str]))
(def counts (frequencies (str/split "a b a c b a" #" ")))
(print (->> counts (sort-by (comp - val)) (map #(str (key %) ":" (val %))) (str/join " ")))
(print "|")
(println (update-in {:stock {:apples 3}} [:stock :apples] + 2) (mod -7 3) (quot 7 2))
