;; EXPECT: boom #<reader closed>
;; with-open closes the handle when its body throws, and the exception propagates.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/l.txt"))
(spit f "x\n")
(def seen (atom nil))
(def msg (try (with-open [r (reader f)]
                (reset! seen r)
                (throw (ex-info "boom" {})))
              (catch ExceptionInfo e (ex-message e))))
(sh "rm" "-rf" dir)
(println msg (replace (str @seen) (str " " f) ""))
