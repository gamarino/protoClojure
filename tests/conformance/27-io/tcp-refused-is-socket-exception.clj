;; EXPECT: SocketException
;; ConnectException is a SocketException, and so an IOException.
(def srv (tcp-listen "127.0.0.1" 0))
(def port (server-port srv))
(close srv)
(println (try (tcp-connect "127.0.0.1" port)
              (catch SocketException e "SocketException")))
