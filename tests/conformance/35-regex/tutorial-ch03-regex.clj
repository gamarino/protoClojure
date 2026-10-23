;; EXPECT: ["JOE@host" "JOE" "host"] ["a" "b" "c"]
;; docs/tutorial/03-for-the-clojure-developer.md §3.3.4: regex.
(prn (re-find #"(?i)(\w+)@(\w+)" "Mail: JOE@host")
     (clojure.string/split "a1b22c" #"\d+"))
