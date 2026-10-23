;; EXPECT: hello true nil (a c) [2026-10 2026 10] .* ab
;; Java's leading inline flags (?i) (?s) (?m), named groups (numbered),
;; \Q...\E quoting, and \A / \z, translated for the engine (D45).
(println (re-find #"(?i)HELLO" "say hello")
         (= (re-find #"(?s)a.b" "a\nb") "a\nb")
         (re-find #"a.b" "a\nb")
         (re-seq #"(?m)^\w" "ab\ncd")
         (re-find #"(?<year>\d{4})-(\d\d)" "on 2026-10 ok")
         (re-find #"\Q.*\E" "x.*y")
         (re-matches #"\Aab\z" "ab"))
