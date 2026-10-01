;; EXPECT: copied: abc
;; with-open binds several handles, each visible to the next, and closes all.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def src (str dir "/in.txt"))
(def dst (str dir "/out.txt"))
(spit src "abc\n")
(with-open [r (reader src)
            w (writer dst)]
  (write w (str "copied: " (read-line r))))
(def text (slurp dst))
(sh "rm" "-rf" dir)
(println text)
