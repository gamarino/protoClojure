;; EXPECT: :handled bcf
;; With a catch that handles the exception, the order is body, catch, finally,
;; and the try's value is the catch's.
(def log (atom ""))
(def r (try (swap! log str "b") (/ 1 0)
            (catch ArithmeticException e (swap! log str "c") :handled)
            (finally (swap! log str "f"))))
(println r @log)
