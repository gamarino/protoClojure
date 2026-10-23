;; EXPECT: "123" ["joe@host" "joe" "host"] nil ("1" "22" "333") ["a" "b" "c"] "smith, john" "a<1>b<2>"
;; LANGUAGE.md §2.10: regular expressions.
(prn (re-find #"\d+" "abc 123 def")
     (re-find #"(\w+)@(\w+)" "mail joe@host")
     (re-matches #"\d+" "123a")
     (re-seq #"\d+" "1 22 333")
     (clojure.string/split "a, b,c" #",\s*")
     (clojure.string/replace "john smith" #"(\w+) (\w+)" "$2, $1")
     (clojure.string/replace "a1b2" #"\d" (fn [d] (str "<" d ">"))))
