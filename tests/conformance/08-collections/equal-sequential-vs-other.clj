;; EXPECT: false false false false false false
;; A list or vector is never = to nil, a string or a map, even when its
;; elements would spell the same content.
(println (= (list) nil)
         (= [] nil)
         (= (list "a" "b") "ab")
         (= [] "")
         (= (list) {})
         (= [:a 1] {:a 1}))
