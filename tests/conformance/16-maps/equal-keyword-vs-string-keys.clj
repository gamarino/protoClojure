;; EXPECT: false false false true
;; Maps and vectors holding a keyword are not = to ones holding the string
;; of its spelling, whether as a key or as a value.
(println (= {:a 1} {":a" 1})
         (= {1 :a} {1 ":a"})
         (= [:a] [":a"])
         (= {:a [:b]} {:a [:b]}))
