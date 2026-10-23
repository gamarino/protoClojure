;; EXPECT: a#b#c smith, john AbC baa caa a$b
;; clojure.string's replace and replace-first with a pattern: $n is group n,
;; \$ a dollar; a fn replacement receives the match.
(println (replace "a1b22c" #"\d+" "#")
         (replace "john smith" #"(\w+) (\w+)" "$2, $1")
         (replace "abc" #"[ac]" (fn [x] (upper-case x)))
         (replace-first "aaa" #"a" "b")
         (replace-first "aaa" "a" "c")
         (replace "a.b" #"\." "\\$"))
