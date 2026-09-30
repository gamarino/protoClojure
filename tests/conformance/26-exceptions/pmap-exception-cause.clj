;; EXPECT: bad 2
(try (pmap (fn [x] (if (= x 2) (throw (ex-info "bad" {:x x})) x)) (list 1 2 3))
     (catch ExecutionException e
       (println (ex-message (ex-cause e)) (:x (ex-data (ex-cause e))))))
