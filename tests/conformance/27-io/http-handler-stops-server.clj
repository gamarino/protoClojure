;; EXPECT: stopping | ConnectException
;; A handler may stop its own server; the request it is answering completes.
(def server (atom nil))
(defn handler [req]
  (stop-server @server)
  {:status 200 :body "stopping"})
(reset! server (run-server handler {:port 0}))
(def url (str "http://127.0.0.1:" (server-port @server) "/"))
(def r (http-get url))
(def after (try (http-get url) (catch ConnectException e "ConnectException")))
(println (:body r) "|" after)
