;; EXPECT: line1 | Stream closed
;; with-open closes the handle when its body returns.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/l.txt"))
(spit f "line1\nline2\n")
(def seen (atom nil))
(def first-line (with-open [r (reader f)] (reset! seen r) (read-line r)))
(def after (try (read-line @seen) (catch IOException e (ex-message e))))
(sh "rm" "-rf" dir)
(println first-line "|" after)
