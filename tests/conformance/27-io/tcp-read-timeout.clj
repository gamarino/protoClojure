;; EXPECT: :timeout
;; :timeout bounds every wait on the socket: a silent peer times out a read.
(def srv (tcp-listen "127.0.0.1" 0))
(defn client []
  (let [s (tcp-connect "127.0.0.1" (server-port srv) {:timeout 200})]
    (try (socket-read-line s)
         (catch SocketTimeoutException e (:type (ex-data e)))
         (finally (socket-close s)))))
(def r (client))
(close srv)
(println r)
