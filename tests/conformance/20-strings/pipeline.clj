;; EXPECT: HELLO,WORLD
(defn shout [s] (upper-case (trim s)))
(println (join "," (map shout (split "  hello , world  " ","))))
