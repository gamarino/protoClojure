;; EXPECT: a/b/c.txt true false true true false true :gone
;; file joins paths; exists?, directory?, make-parents and delete-file act on them.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (file dir "a" "b" "c.txt"))
(def made (make-parents f))
(spit f "x")
(def exists-before (exists? f))
(def is-dir (directory? f))
(def parent-dir (directory? (file dir "a" "b")))
(def deleted (delete-file f))
(def exists-after (exists? f))
(def silently (delete-file f :gone))
(sh "rm" "-rf" dir)
(println (subs (str f) (+ 1 (count dir))) made is-dir exists-before deleted exists-after parent-dir silently)
