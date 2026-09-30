;; EXPECT: hello udp from 127.0.0.1 true
;; A UDP datagram sent on loopback is received with its sender.
(def a (udp-socket "127.0.0.1" 0))
(def b (udp-socket "127.0.0.1" 0))
(udp-send a "127.0.0.1" (server-port b) "hello udp")
(def d (udp-receive b 5000))
(println (:data d) "from" (:host d) (= (:port d) (server-port a)))
(close a)
(close b)
