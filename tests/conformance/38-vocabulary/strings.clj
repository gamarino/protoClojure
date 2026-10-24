;; EXPECT: a,b x [a b c] AB ab true true true
;; The clojure.string functions a script uses, through the str alias.
(require '[clojure.string :as str])
(println (str/join "," ["a" "b"]) (str/trim "  x ") (str/split-lines "a\nb\r\nc\n\n") (str/upper-case "ab")
         (str/lower-case "AB") (str/starts-with? "abc" "ab") (str/ends-with? "abc" "bc") (str/includes? "abc" "b"))
