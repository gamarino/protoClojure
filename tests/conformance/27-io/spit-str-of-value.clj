;; EXPECT: [1 "a" :k] 2.5
;; spit writes (str content): collections print readably, nil is empty.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/a.txt"))
(spit f [1 "a" :k])
(spit f " " :append true)
(spit f nil :append true)
(spit f 2.5 :append true)
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
