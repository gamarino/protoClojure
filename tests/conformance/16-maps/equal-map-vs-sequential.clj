;; EXPECT: false false false false false
;; A map is never = to a vector, a list or nil.
(println (= {} [])
         (= {:a 1} [:a 1])
         (= [[:a 1]] {:a 1})
         (= {:a 1} (list :a 1))
         (= {} nil))
