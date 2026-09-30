;; EXPECT: 3 (alpha beta gamma) true
;; line-seq answers the lines of a reader; nil for an empty one.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/l.txt"))
(def e (str dir "/empty.txt"))
(spit f "alpha\nbeta\ngamma")
(spit e "")
(def lines (with-open [r (reader f)] (line-seq r)))
(def none (with-open [r (reader e)] (line-seq r)))
(sh "rm" "-rf" dir)
(println (count lines) lines (nil? none))
