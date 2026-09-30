;; EXPECT: 200 text/plain hello /greet name=ana%20b :get
;; run-server calls the Ring handler; http-get answers {:status :headers :body}.
(defn handler [req]
  {:status 200
   :headers {"Content-Type" "text/plain"}
   :body (str "hello " (:uri req) " " (:query-string req) " " (:request-method req))})
(def s (run-server handler {:port 0}))
(def r (http-get (str "http://127.0.0.1:" (server-port s) "/greet?name=ana%20b")))
(stop-server s)
(println (:status r) (get (:headers r) "content-type") (:body r))
