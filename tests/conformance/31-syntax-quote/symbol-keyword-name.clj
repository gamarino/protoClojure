;; EXPECT: foo ns/x true :k :ns/k "x" "k" "s"
;; symbol, keyword and name convert between strings, symbols and keywords.
(println (symbol "foo") (symbol "ns" "x") (= (symbol "a") 'a)
         (keyword "k") (keyword "ns" "k")
         (pr-str (name 'ns/x)) (pr-str (name :k)) (pr-str (name "s")))
