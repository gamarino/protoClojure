;; EXPECT: a [b c] {:k v} (x :y) #<atom s> nil
;; println renders strings without quotes at every depth, and nil as nil.
(println "a" ["b" "c"] {:k "v"} (list "x" :y) (atom "s") nil)
