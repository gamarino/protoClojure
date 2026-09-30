;; EXPECT: 2
;; with-open works inside a fn body, closing over its locals.
(defn count-lines [path]
  (with-open [r (reader path)]
    (count (line-seq r))))
(def dir (trim (:out (sh "mktemp" "-d"))))
(spit (str dir "/f") "a\nb\n")
(def n (count-lines (str dir "/f")))
(sh "rm" "-rf" dir)
(println n)
