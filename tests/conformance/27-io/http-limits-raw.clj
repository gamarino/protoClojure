;; EXPECT: 400 414 431 431 413 200
;; Malformed or oversized requests are refused before the handler runs.
(defn handler [req] {:status 200 :body "ok"})
(def s (run-server handler {:port 0 :max-body 1000}))
(defn status-of [text]
  (let [c (tcp-connect "127.0.0.1" (server-port s) {:timeout 5000})]
    (socket-write c text)
    (let [line (socket-read-line c)]
      (socket-close c)
      (nth (split line " ") 1))))
(defn repeat-str [s n]
  (loop [acc "" i 0] (if (< i n) (recur (str acc s) (+ i 1)) acc)))
(defn doubled [s k] (loop [acc s i 0] (if (< i k) (recur (str acc acc) (+ i 1)) acc)))
(def long-path (doubled "a" 14))
(def long-value (doubled "v" 14))
(def many-headers (repeat-str "x-h: 1\r\n" 101))
(println (status-of "GARBAGE\r\n\r\n")
         (status-of (str "GET /" long-path " HTTP/1.1\r\n\r\n"))
         (status-of (str "GET / HTTP/1.1\r\nx-big: " long-value "\r\n\r\n"))
         (status-of (str "GET / HTTP/1.1\r\n" many-headers "\r\n"))
         (status-of "POST / HTTP/1.1\r\ncontent-length: 5000\r\n\r\n")
         (status-of "GET / HTTP/1.1\r\nhost: x\r\n\r\n"))
(stop-server s)
