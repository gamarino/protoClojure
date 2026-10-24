;; EXPECT-ERROR: IOException: Stream closed
;; A lazy line-seq returned out of with-open is read after the reader has
;; closed, as in Clojure: doall it inside.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/l.txt"))
(spit f "a\nb\nc\n")
(def lines (with-open [r (reader f)] (line-seq r)))
(sh "rm" "-rf" dir)
(println (count lines))
