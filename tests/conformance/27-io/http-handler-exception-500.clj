;; EXPECT: 500 Internal Server Error | still serving
;; A handler that throws answers a plain 500, and the server keeps serving.
(defn handler [req]
  (if (= (:uri req) "/boom")
    (throw (ex-info "handler failed" {}))
    {:status 200 :body "still serving"}))
(def s (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port s)))
(def r (http-get (str base "/boom") {:throw false}))
(def ok (http-get (str base "/ok")))
(stop-server s)
(println (:status r) (:body r) "|" (:body ok))
