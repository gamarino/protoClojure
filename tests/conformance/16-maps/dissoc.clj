;; EXPECT: {:b 2} {} true true nil {:a 1} true
;; dissoc returns the map without the given keys. An absent key leaves an
;; equal map, (dissoc m) is m, (dissoc nil k) is nil, and the original map is
;; unchanged.
(def m {:a 1 :b 2 :c 3})
(println (dissoc {:a 1 :b 2} :a)
         (dissoc m :a :b :c)
         (= (dissoc m :zz) m)
         (= (dissoc m :b) {:a 1 :c 3})
         (dissoc nil :a)
         (dissoc {:a 1})
         (= m {:a 1 :b 2 :c 3}))
