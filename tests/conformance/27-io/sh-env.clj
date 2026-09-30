;; EXPECT: [bar]
;; :env replaces the program's environment.
(println (str "[" (trim (:out (sh "sh" "-c" "echo $FOO" :env {"FOO" "bar"}))) "]"))
