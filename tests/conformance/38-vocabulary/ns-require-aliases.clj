;; EXPECT: a-b-c X true true [x y]
;; ns with (:require [clojure.string :as str] [clojure.set :as set]):
;; str/ and set/ name the clojure.string and clojure.set functions.
(ns my.script
  (:require [clojure.string :as str]
            [clojure.set :as set]))
(println (str/join "-" ["a" "b" "c"]) (str/upper-case "x") (= #{1 2} (set/union #{1} #{2}))
         (str/starts-with? "abc" "a") (str/split "x,y" #","))
