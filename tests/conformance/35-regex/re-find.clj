;; EXPECT: 123 [joe@host joe host] nil [a a nil]
;; re-find answers the first match: the string, or a vector of the match
;; and its groups (nil for a group that did not take part); nil when none.
(println (re-find #"\d+" "abc 123 def 45")
         (re-find #"(\w+)@(\w+)" "mail: joe@host ok")
         (re-find #"x" "abc")
         (re-find #"(a)(b)?" "xa"))
