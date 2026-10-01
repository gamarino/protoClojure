;; EXPECT: hello, file
;; spit writes a file and slurp reads it back.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/a.txt"))
(spit f "hello, file")
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
