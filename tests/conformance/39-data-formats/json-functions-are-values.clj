;; EXPECT: [[1] [2] {a 1}] #<fn parse-string>
;; The namespace functions are values: passed to map, printed by name.
(require '[cheshire.core :as json])
(println (vec (map json/parse-string ["[1]" "[2]" "{\"a\":1}"])) json/parse-string)
