;; EXPECT: a, b|a,b
;; Repeated request headers reach the handler joined with a comma, as in Ring.
(defn handler [req] {:status 200 :body (get (:headers req) "x-multi")})
(def s (run-server handler {:port 0}))
(defn raw []
  (let [c (tcp-connect "127.0.0.1" (server-port s) {:timeout 5000})]
    (socket-write c "GET / HTTP/1.1\r\nhost: x\r\nx-multi: a\r\nx-multi: b\r\n\r\n")
    (let [lines (line-seq c)] (socket-close c) (last-of lines))))
(defn last-of [l] (if (empty? (rest l)) (first l) (last-of (rest l))))
(def body (raw))
(def viaclient (:body (http-get (str "http://127.0.0.1:" (server-port s) "/") {:headers {"x-multi" "a, b"}})))
(stop-server s)
(println (str viaclient "|" body))
