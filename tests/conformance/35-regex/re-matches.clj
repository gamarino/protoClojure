;; EXPECT: 123 nil [2026-10 2026 10]
;; re-matches matches the whole string.
(println (re-matches #"\d+" "123") (re-matches #"\d+" "123a")
         (re-matches #"(\d{4})-(\d\d)" "2026-10"))
