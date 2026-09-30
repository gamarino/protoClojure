;; EXPECT: 3
;; A handler may use actors and futures: they run on their own threads.
(def counter (actor 0))
(defn handler [req]
  (let [n @(send counter (fn [s] (+ s 1)))
        doubled @(future (* n 1))]
    {:status 200 :body (str doubled)}))
(def s (run-server handler {:port 0}))
(def url (str "http://127.0.0.1:" (server-port s) "/"))
(http-get url)
(http-get url)
(def r (http-get url))
(stop-server s)
(println (:body r))
