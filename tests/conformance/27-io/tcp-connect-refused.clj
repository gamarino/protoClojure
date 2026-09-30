;; EXPECT: ConnectException :connection-refused
;; Connecting to a closed port throws ConnectException.
(def srv (tcp-listen "127.0.0.1" 0))
(def port (server-port srv))
(close srv)
(println (try (tcp-connect "127.0.0.1" port {:timeout 5000})
              (catch ConnectException e (str "ConnectException " (:type (ex-data e))))))
