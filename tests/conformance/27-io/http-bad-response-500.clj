;; EXPECT: 500 500 200
;; A response header with a line break, or an answer that is not a map, becomes a plain 500.
(defn handler [req]
  (cond (= (:uri req) "/inject") {:status 200 :headers {"x-a" "1\r\nset-cookie: evil=1"} :body "x"}
        (= (:uri req) "/nonmap") "just a string"
        :else {:body "fine"}))
(def s (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port s)))
(def a (http-get (str base "/inject") {:throw false}))
(def b (http-get (str base "/nonmap") {:throw false}))
(def c (http-get (str base "/other")))
(stop-server s)
(println (:status a) (:status b) (:status c))
