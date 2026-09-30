;; EXPECT: from-finally
;; An exception raised by finally replaces the one in flight, as in Java.
(try
  (try (throw (ex-info "from-body" {}))
       (finally (throw (ex-info "from-finally" {}))))
  (catch Exception e (println (ex-message e))))
