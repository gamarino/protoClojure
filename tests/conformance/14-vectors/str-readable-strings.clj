;; EXPECT: a["b" "c\"d\\e\n"]{:k "v"}("x" :y 1)#<atom "s">
;; str inserts a top-level string as is and renders the strings nested in a
;; collection readably: quoted, with ", \ and control characters escaped.
(println (str "a" ["b" "c\"d\\e\n"] {:k "v"} (list "x" :y 1) (atom "s")))
