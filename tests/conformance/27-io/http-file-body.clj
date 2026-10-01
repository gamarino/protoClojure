;; EXPECT: from a file
;; A handler may answer a file as its body.
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(spit (str dir "/page.txt") "from a file")
(def s (run-server (fn [req] {:body (file dir "page.txt")}) {:port 0}))
(def r (http-get (str "http://127.0.0.1:" (server-port s) "/")))
(stop-server s)
(sh "rm" "-rf" dir)
(println (:body r))
