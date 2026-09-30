;; EXPECT: hello, file
;; spit writes a file and slurp reads it back.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/a.txt"))
(spit f "hello, file")
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
