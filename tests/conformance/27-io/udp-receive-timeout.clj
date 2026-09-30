;; EXPECT: nil
;; udp-receive with a timeout answers nil when nothing arrives.
(def s (udp-socket "127.0.0.1" 0))
(def d (udp-receive s 100))
(close s)
(println (if (nil? d) "nil" d))
