;; EXPECT: 1 nil 42 :none 1 (2 1 nil)
;; A map is a function of its keys: (m k) is (get m k) and (m k not-found) is
;; (get m k not-found). A keyword not-found argument goes through CALL_KW.
(def m {:a 1})
(def sm {"x" 1})
(println ({:a 1} :a)
         (m :b)
         (m :b 42)
         (m :b :none)
         (sm "x")
         (map {:a 1 :b 2} [:b :a :c]))
