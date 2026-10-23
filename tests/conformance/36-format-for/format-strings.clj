;; EXPECT: abc|   ab|ab   |ab|null :kw [1 "a"] {:a 1}|ABC
;; format %s: width, left justification, precision; nil is "null" and other
;; values render as str renders them, as Java's String.format with Clojure
;; values; %S upper-cases.
(println (str (format "%s|%5s|%-5s|%.2s" "abc" "ab" "ab" "abcdef") "|"
              (format "%s %s %s %s" nil :kw [1 "a"] {:a 1}) "|"
              (format "%S" "abc")))
