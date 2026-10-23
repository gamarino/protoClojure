;; EXPECT: true true true true
;; Inside a syntax-quote, `name#` is one generated symbol, `name__N__auto__`,
;; the same everywhere in that syntax-quote; another `name#`, or the same
;; spelling in another syntax-quote, is a different one.
(def v `[a# a# b#])
(def w `[a#])
(println (= (nth v 0) (nth v 1))
         (not= (nth v 0) (nth v 2))
         (not= (nth v 0) (nth w 0))
         (and (starts-with? (str (nth v 0)) "a__") (ends-with? (str (nth v 0)) "__auto__")))
