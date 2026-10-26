;; EXPECT: "42" "y" "aaab" nil ["33" "3"] ["ÉT" "é"] "é" 2 "abc" ["a" "b" "c"] ("" "") "-a-b-c-" true
;; java.util.regex constructs the engine reads as Java does (M48):
;; lookbehind, possessive quantifiers, named back-references, Unicode
;; properties and \uXXXX escapes, `.` as one character, inline flags in the
;; middle of a pattern, (?x) comments, Java's POSIX classes, and Java's walk
;; over successive empty matches.
(prn (re-find #"(?<=\$)\d+" "cost $42")
     (re-find #"(?<!x)y" "xy ay")
     (re-find #"a++b" "aaab")
     (re-find #"a++a" "aaaa")
     (re-find #"(?<d>\d)\k<d>" "12 33")
     [(re-find #"\p{Lu}+" "abcÉTé") (re-find #"é" "café")]
     (re-find #"^.$" "é")
     (count (re-seq #"(?i)a(?-i)B" "aB AB ab"))
     (re-find #"(?x) a b # a comment
                 c" "xabc")
     (clojure.string/split "a!b?c" #"\p{Punct}")
     (re-seq #"|a" "a")
     (clojure.string/replace "abc" #"" "-")
     (= ["2026-10" "2026" "10"] (re-matches #"(?<y>\d{4})-(?<m>\d\d)" "2026-10")))
