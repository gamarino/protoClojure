;; EXPECT: ñ o true
;; nth indexes a string by code point, as count and subs do.
(def s "año")
(println (nth s 1) (nth s 2) (= (nth s 1) (subs s 1 2)))
