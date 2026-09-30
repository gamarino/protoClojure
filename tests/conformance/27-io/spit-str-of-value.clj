;; EXPECT: [1 "a" :k] 2.5
;; spit writes (str content): collections print readably, nil is empty.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/a.txt"))
(spit f [1 "a" :k])
(spit f " " :append true)
(spit f nil :append true)
(spit f 2.5 :append true)
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
