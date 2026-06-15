;; EXPECT: ((:tracker 1 2) (:tracker 0 1))
(def a (atom 0))
(def log (atom (list)))
(add-watch a :tracker
  (fn [k atom old new]
    (swap! log (fn [l] (cons (list k old new) l)))))
(reset! a 1)
(swap! a inc)
(println @log)
