;; EXPECT: 200 añb€ 4
;; A chunked response whose chunks split multi-byte UTF-8 characters is
;; decoded as bytes: the body arrives whole. The raw server is a small
;; python3 program, since a protoClojure string cannot hold half a character.
(def probe (tcp-listen "127.0.0.1" 0))
(def port (server-port probe))
(close probe)
(def script (str "import socket\n"
                 "s = socket.socket()\n"
                 "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
                 "s.bind(('127.0.0.1', " port "))\n"
                 "s.listen(1)\n"
                 "c, _ = s.accept()\n"
                 "buf = b''\n"
                 "while b'\\r\\n\\r\\n' not in buf:\n"
                 "    buf += c.recv(4096)\n"
                 "body = 'añb€'.encode()\n"
                 "parts = [body[0:2], body[2:4], body[4:6], body[6:]]\n"
                 "out = b'HTTP/1.1 200 OK\\r\\ntransfer-encoding: chunked\\r\\n\\r\\n'\n"
                 "for p in parts:\n"
                 "    out += b'%x\\r\\n' % len(p) + p + b'\\r\\n'\n"
                 "out += b'0\\r\\n\\r\\n'\n"
                 "c.sendall(out)\n"
                 "c.close()\n"))
(def server (future (sh "python3" "-c" script)))
(defn fetch [tries]
  (let [r (try (http-get (str "http://127.0.0.1:" port "/") {:timeout 5000})
               (catch ConnectException e nil))]
    (if (or r (= tries 0))
      r
      (do (sh "sleep" "0.05") (fetch (- tries 1))))))
(def r (fetch 100))
@server
(println (:status r) (:body r) (count (:body r)))
