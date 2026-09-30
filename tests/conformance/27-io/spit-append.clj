;; EXPECT: one|two|three
;; spit :append true adds to the end; a plain spit replaces the contents.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/a.txt"))
(spit f "zero|")
(spit f "one|")
(spit f "two|" :append true)
(spit f "three" :append true)
(def text (slurp f))
(sh "rm" "-rf" dir)
(println text)
