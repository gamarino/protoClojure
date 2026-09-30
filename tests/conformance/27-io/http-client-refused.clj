;; EXPECT: ConnectException
;; A network failure throws the matching IOException subclass.
(def srv (tcp-listen "127.0.0.1" 0))
(def port (server-port srv))
(close srv)
(println (try (http-get (str "http://127.0.0.1:" port "/"))
              (catch ConnectException e "ConnectException")))
