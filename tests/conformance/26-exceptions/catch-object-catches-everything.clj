;; EXPECT: :object :throwable
;; `Object` and `Throwable` catch every thrown value.
(println (try (/ 1 0) (catch Object e :object))
         (try (throw (ex-info "x" {})) (catch java.lang.Throwable e :throwable)))
