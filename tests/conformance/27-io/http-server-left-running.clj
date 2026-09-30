;; EXPECT: served
;; A server still running when the script ends is stopped at exit.
(def s (run-server (fn [req] {:body "served"}) {:port 0}))
(println (:body (http-get (str "http://127.0.0.1:" (server-port s) "/"))))
