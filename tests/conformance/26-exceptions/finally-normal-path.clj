;; EXPECT: 1 bfa
;; finally runs after a body that completes normally; the try's value is the
;; body's, not the finally's.
(def log (atom ""))
(def r (try (swap! log str "b") 1
            (finally (swap! log str "f") 2)))
(swap! log str "a")
(println r @log)
