;; EXPECT: echo: ping | server saw ping
;; A TCP server and client on loopback exchange lines.
(def srv (tcp-listen "127.0.0.1" 0))
(def port (server-port srv))
(def server
  (future
    (let [c (tcp-accept srv 5000)
          line (socket-read-line c)]
      (socket-write c (str "echo: " line "\n"))
      (socket-close c)
      line)))
(defn client []
  (let [s (tcp-connect "127.0.0.1" port {:timeout 5000})]
    (socket-write s "ping\n")
    (let [reply (socket-read-line s)]
      (socket-close s)
      reply)))
(def reply (client))
(def seen @server)
(close srv)
(println reply "| server saw" seen)
