;; EXPECT: 413
;; A chunked request body over :max-body is refused with 413.
(defn handler [req] {:status 200 :body "ok"})
(def s (run-server handler {:port 0 :max-body 10}))
(defn status-of [text]
  (let [c (tcp-connect "127.0.0.1" (server-port s) {:timeout 5000})]
    (socket-write c text)
    (let [line (socket-read-line c)]
      (socket-close c)
      (nth (split line " ") 1))))
(println (status-of "POST / HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n14\r\n01234567890123456789\r\n0\r\n\r\n"))
(stop-server s)
