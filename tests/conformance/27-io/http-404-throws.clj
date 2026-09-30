;; EXPECT: Exceptional status code: 404 404 not here | 404
;; A non-2xx status throws ex-info carrying the response, unless :throw false.
(defn handler [req] {:status 404 :body "not here"})
(def s (run-server handler {:port 0}))
(def url (str "http://127.0.0.1:" (server-port s) "/missing"))
(def thrown (try (http-get url)
                 (catch ExceptionInfo e (str (ex-message e) " " (:status (ex-data e)) " " (:body (ex-data e))))))
(def plain (http-get url {:throw false}))
(stop-server s)
(println thrown "|" (:status plain))
