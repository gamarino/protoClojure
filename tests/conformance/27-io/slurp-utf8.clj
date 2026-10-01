;; EXPECT: año ñandú 日本 9
;; Text round-trips as UTF-8; count counts characters.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/u.txt"))
(spit f "año ñandú 日本")
(def text (slurp f :encoding "UTF-8"))
(sh "rm" "-rf" dir)
(println text (count (subs text 0 9)))
