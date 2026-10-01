;; EXPECT: a1b2
;; writer truncates the file; writer with :append true adds to it.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/w.txt"))
(spit f "old contents")
(with-open [w (writer f)] (write w "a") (write w 1))
(with-open [w (writer f :append true)] (write w "b") (write w 2))
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
