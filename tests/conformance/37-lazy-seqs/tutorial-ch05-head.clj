;; EXPECT: nil 1000000
;; Tutorial chapter 5, §5.9: walking without the head, and holding it.
(def a (dorun (map inc (range 1000000))))
(def b (let [s (map inc (range 1000000))]
  (dorun s) (count s)))
(println a b)
