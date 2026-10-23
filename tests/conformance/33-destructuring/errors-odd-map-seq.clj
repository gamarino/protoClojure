;; EXPECT-ERROR: IllegalArgumentException: No value supplied for key: :b
;; A rest destructured as a map needs key/value pairs.
(defn f [& {:as m}] m)
(f :a 1 :b)
