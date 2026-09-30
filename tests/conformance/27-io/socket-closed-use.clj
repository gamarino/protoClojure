;; EXPECT: Stream closed
;; Using a closed socket throws IOException; closing twice is harmless.
(def srv (tcp-listen "127.0.0.1" 0))
(def s (tcp-connect "127.0.0.1" (server-port srv)))
(socket-close s)
(socket-close s)
(close srv)
(println (try (socket-write s "x") (catch IOException e (ex-message e))))
