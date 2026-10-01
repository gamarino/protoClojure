;; EXPECT: true
;; :dir runs the program in that directory.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def r (sh "pwd" :dir dir))
(sh "rm" "-rf" dir)
(println (ends-with? (trim (:out r)) (trim (:out (sh "basename" dir)))))
