;; EXPECT: <> ab 0 [nil] (nil 1)
;; str renders nil as the empty string, at any argument position, while a
;; nil inside a collection still prints as nil.
(println (str "<" (str nil) ">") (str "a" nil "b") (count (str nil))
         (str [nil]) (str (list nil 1)))
