;; EXPECT: q=a%20b&n=1
;; :query-params are percent-encoded into the URL.
(def s (run-server (fn [req] {:body (:query-string req)}) {:port 0}))
(def r (http-get (str "http://127.0.0.1:" (server-port s) "/") {:query-params {"q" "a b"}}))
(def r2 (http-request {:uri (str "http://127.0.0.1:" (server-port s) "/?q=a%20b") :query-params {:n 1}}))
(stop-server s)
(println (:body r2))
