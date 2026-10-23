;; EXPECT: 2 true nil | true #{2}
;; README.md, "A flavour of the language": sets (the two printed lines are
;; joined here, so the runner checks both).
(def seen (into #{} ["a" "b" "a"]))
(print (count seen) (contains? seen "a") (seen "z") "| ")
(println (= (union #{1 2} #{3}) #{1 2 3}) (intersection #{1 2} #{2 3}))
