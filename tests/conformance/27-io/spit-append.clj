;; EXPECT: one|two|three
;; spit :append true adds to the end; a plain spit replaces the contents.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/a.txt"))
(spit f "zero|")
(spit f "one|")
(spit f "two|" :append true)
(spit f "three" :append true)
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
