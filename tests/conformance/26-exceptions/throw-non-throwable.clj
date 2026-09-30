;; EXPECT: :cce
;; Only exceptions can be thrown; throwing another value raises a
;; ClassCastException, as in JVM Clojure.
(println (try (throw 42) (catch ClassCastException e :cce)))
