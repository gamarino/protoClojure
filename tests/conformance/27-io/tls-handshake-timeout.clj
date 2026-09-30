;; EXPECT: plain connect ok | tls SocketTimeoutException
;; A listener that accepts but never speaks: a plain connect succeeds, while a
;; TLS handshake is bounded by :timeout and throws SocketTimeoutException.
(def srv (tcp-listen "127.0.0.1" 0))
(def port (server-port srv))
(defn plain []
  (let [s (tcp-connect "127.0.0.1" port {:timeout 2000})]
    (socket-close s)
    "plain connect ok"))
(defn tls []
  (try (tcp-connect "127.0.0.1" port {:tls true :timeout 300})
       "tls connected?"
       (catch SocketTimeoutException e "tls SocketTimeoutException")))
(def a (plain))
(def b (tls))
(close srv)
(println a "|" b)
