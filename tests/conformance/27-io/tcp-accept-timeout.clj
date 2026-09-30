;; EXPECT: nil
;; tcp-accept with a timeout answers nil when nobody connects.
(def srv (tcp-listen "127.0.0.1" 0))
(def c (tcp-accept srv 100))
(close srv)
(println (if (nil? c) "nil" c))
