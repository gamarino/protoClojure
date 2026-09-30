;; EXPECT: 2 a=1 b=2
;; A vector of values repeats a response header.
(defn handler [req]
  {:status 200 :headers {"set-cookie" ["a=1" "b=2"]} :body "x"})
(def s (run-server handler {:port 0}))
(defn raw-lines []
  (let [c (tcp-connect "127.0.0.1" (server-port s) {:timeout 5000})]
    (socket-write c "GET / HTTP/1.1\r\nhost: x\r\n\r\n")
    (let [lines (line-seq c)] (socket-close c) lines)))
(def lines (raw-lines))
(stop-server s)
(def cookies (filter (fn [l] (starts-with? (lower-case l) "set-cookie")) lines))
(println (count cookies) (trim (subs (first cookies) 11)) (trim (subs (first (rest cookies)) 11)))
