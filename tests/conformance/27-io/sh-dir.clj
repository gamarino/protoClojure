;; EXPECT: true
;; :dir runs the program in that directory.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def r (sh "pwd" :dir dir))
(sh "rm" "-rf" dir)
(println (ends-with? (trim (:out r)) (trim (:out (sh "basename" dir)))))
