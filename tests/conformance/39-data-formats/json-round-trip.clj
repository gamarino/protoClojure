;; EXPECT: :ok
;; generate then parse gives the data back, for both APIs (keyword keys, and
;; every value JSON can hold).
(require '[cheshire.core :as c] '[clojure.data.json :as d])
(def data {:id 12345678901234567890 :name "Zoë \"q\" \\ /" :tags ["a" "b"] :score 1.25
           :nested {:list [1 [2 [3]] {}] :none nil :yes true :no false}})
(println (if (and (= data (c/parse-string (c/generate-string data) true))
                  (= data (c/parse-string (c/generate-string data {:pretty true :escape-non-ascii true}) true))
                  (= data (d/read-str (d/write-str data) :key-fn keyword)))
           :ok :mismatch))
