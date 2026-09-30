;; EXPECT: 16 16 true
;; Concurrent clients are served, each request on its own thread.
(def hits (atom 0))
(defn handler [req]
  (swap! hits inc)
  {:status 200 :body (str "n=" (:query-string req))})
(def s (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port s) "/?"))
(def bodies (pmap (fn [i] (:body (http-get (str base i))))
                  (list 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)))
(stop-server s)
(println (count bodies) @hits (= (first bodies) "n=1"))
