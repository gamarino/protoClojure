;; EXPECT: 0 ["LINE ONE\nLINE TWO\n"]
;; :in is fed to the program's standard input.
(def r (sh "tr" "a-z" "A-Z" :in "line one\nline two\n"))
(println (:exit r) (str [(:out r)]))
