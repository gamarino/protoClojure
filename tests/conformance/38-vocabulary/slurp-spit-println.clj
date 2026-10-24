;; EXPECT: hello world {:a [1 x], :b #{2}} #<atom 1> nil true
;; slurp and spit, and println of any value.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/x.txt"))
(spit f "hello")
(spit f " world" :append true)
(println (slurp f) {:a [1 "x"] :b #{2}} (atom 1) nil true)
(sh "rm" "-rf" dir)
