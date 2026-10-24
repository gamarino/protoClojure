;; EXPECT: (1 4 9) 6 true {:a {:b 11}} 2 1 ("a" "bb" "ccc") a, b #<file a/b>
;; The Part C examples of LANGUAGE.md §2.8, §6.2 and §7.1, as written there.
(ns my.script
  (:require [clojure.string :as str]
            [clojure.set :as set]))
(def a (map #(* % %) [1 2 3]))
(def b (#(apply + %&) 1 2 3))
(def c (letfn [(ev? [n] (if (= n 0) true (od? (dec n))))
               (od? [n] (if (= n 0) false (ev? (dec n))))]
         (ev? 10)))
(def d (update-in {:a {:b 1}} [:a :b] + 10))
(def e (->> "a b a" (re-seq #"\w") frequencies))
(def f (sort-by count ["ccc" "a" "bb"]))
(def g (str/join ", " (sort (set/union #{"b"} #{"a"}))))
(require '[clojure.java.io :as io])
(println a b c d (e "a") (e "b") (pr-str f) g (io/file "a" "b"))
