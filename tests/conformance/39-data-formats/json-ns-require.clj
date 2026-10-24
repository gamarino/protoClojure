;; EXPECT: [{:a 1} "{\"a\":1}" [1]]
;; The JSON and EDN namespaces are known to ns / require, so aliases work.
(ns showcase.json
  (:require [cheshire.core :as json]
            [clojure.data.json :as dj]
            [clojure.edn :as edn]))
(prn [(json/parse-string "{\"a\":1}" true) (dj/write-str {:a 1}) (edn/read-string "[1]")])
