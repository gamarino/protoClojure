;; EXPECT: ok | ConnectException
;; After stop-server, connections are refused.
(def s (run-server (fn [req] {:body "ok"}) {:port 0}))
(def port (server-port s))
(def before (:body (http-get (str "http://127.0.0.1:" port "/"))))
(stop-server s)
(def after (try (http-get (str "http://127.0.0.1:" port "/"))
                (catch ConnectException e "ConnectException")))
(println before "|" after)
