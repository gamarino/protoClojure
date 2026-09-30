;; EXPECT: ExecutionException {:k 1} x
;; deref of a failed future throws an ExecutionException whose cause is the
;; exception the body raised, as in JVM Clojure.
(def f (future (throw (ex-info "x" {:k 1}))))
(try @f
     (catch ExecutionException e
       (println "ExecutionException" (ex-data (ex-cause e)) (ex-message (ex-cause e)))))
