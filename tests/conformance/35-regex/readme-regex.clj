;; EXPECT: [joe@host joe host] | (1 22 333) | [a b c] | smith, john
;; README.md, "A flavour of the language": regular expressions (the four
;; printed lines are joined here, so the runner checks them all).
(print (re-find #"(\w+)@(\w+)" "mail joe@host") "| ")
(print (re-seq #"\d+" "1 22 333") "| ")
(print (clojure.string/split "a, b,c" #",\s*") "| ")
(println (clojure.string/replace "john smith" #"(\w+) (\w+)" "$2, $1"))
