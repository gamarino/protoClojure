;; EXPECT: 2
;; with-open works inside a fn body, closing over its locals.
(defn count-lines [path]
  (with-open [r (reader path)]
    (count (line-seq r))))
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(spit (str dir "/f") "a\nb\n")
(def n (count-lines (str dir "/f")))
(sh "rm" "-rf" dir)
(println n)
