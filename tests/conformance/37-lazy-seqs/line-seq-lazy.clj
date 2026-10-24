;; EXPECT: alpha (beta gamma) 3
;; line-seq reads lines as they are consumed; inside with-open it can be
;; read part by part.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/l.txt"))
(spit f "alpha\nbeta\ngamma\n")
(with-open [r (reader f)]
  (let [s (line-seq r)]
    (println (first s) (rest s) (count s))))
(sh "rm" "-rf" dir)
