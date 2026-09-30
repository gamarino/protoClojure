;; EXPECT: año ñandú 日本 9
;; Text round-trips as UTF-8; count counts characters.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/u.txt"))
(spit f "año ñandú 日本")
(def text (slurp f :encoding "UTF-8"))
(sh "rm" "-rf" dir)
(println text (count (subs text 0 9)))
