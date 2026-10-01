;; EXPECT: boom #<reader closed>
;; with-open closes the handle when its body throws, and the exception propagates.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/l.txt"))
(spit f "x\n")
(def seen (atom nil))
(def msg (try (with-open [r (reader f)]
                (reset! seen r)
                (throw (ex-info "boom" {})))
              (catch ExceptionInfo e (ex-message e))))
(sh "rm" "-rf" dir)
(println msg (replace (str @seen) (str " " f) ""))
