;; EXPECT: 3 (alpha beta gamma) true
;; line-seq answers the lines of a reader; nil for an empty one.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/l.txt"))
(def e (str dir "/empty.txt"))
(spit f "alpha\nbeta\ngamma")
(spit e "")
(def lines (with-open [r (reader f)] (line-seq r)))
(def none (with-open [r (reader e)] (line-seq r)))
(sh "rm" "-rf" dir)
(println (count lines) lines (nil? none))
