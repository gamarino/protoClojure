;; EXPECT: 1 nil 2 nil 9
;; A keyword is a function of a map: (:k m) is (get m :k). The keyword never
;; matches the string of its spelling, and calls nest.
(println (:a {:a 1})
         (:a {":a" 1})
         (:b {:a 1 :b 2})
         (:c {:a 1})
         (:b (:a {:a {:b 9}})))
