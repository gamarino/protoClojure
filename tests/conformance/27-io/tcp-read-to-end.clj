;; EXPECT: (a b c) nil
;; line-seq reads a socket to its end; read-line then answers nil.
(def srv (tcp-listen "127.0.0.1" 0))
(def server
  (future
    (let [c (tcp-accept srv 5000)]
      (socket-write c "a\nb\nc\n")
      (socket-close c))))
(defn client []
  (let [s (tcp-connect "127.0.0.1" (server-port srv))
        lines (line-seq s)
        after (socket-read-line s)]
    (socket-close s)
    [lines after]))
(def r (client))
@server
(close srv)
(println (first r) (if (nil? (nth r 1)) "nil" "not nil"))
