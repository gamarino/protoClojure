;; EXPECT: text|text|line
;; copy takes a string, a file or a reader and writes a file or a writer.
(def dir (trim (:out (sh "mktemp" "-d"))))
(copy "text" (file dir "a"))
(copy (file dir "a") (file dir "b"))
(spit (str dir "/c") "line\n")
(with-open [r (reader (str dir "/c"))
            w (writer (str dir "/d"))]
  (copy r w))
(def out (str (slurp (str dir "/a")) "|" (slurp (str dir "/b")) "|" (trim (slurp (str dir "/d")))))
(sh "rm" "-rf" dir)
(println out)
