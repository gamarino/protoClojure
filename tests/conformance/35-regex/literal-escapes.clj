;; EXPECT: "hi" 3 a.b
;; In a #"..." literal a backslash stays with the next character, as in
;; Clojure: #"\"" is the pattern \" (a quote), #"\d" a digit, #"\." a dot.
(println (re-find #"\"\w+\"" "say \"hi\"") (count (re-find #"\d+" "x123")) (re-find #"a\.b" "a.b axb"))
