;; EXPECT: true true true true
;; gensym answers a fresh symbol each call: G__N, or the prefix followed by N.
(def a (gensym))
(def b (gensym "tmp"))
(println (symbol? a) (starts-with? (str a) "G__") (starts-with? (str b) "tmp")
         (not= (gensym "x") (gensym "x")))
