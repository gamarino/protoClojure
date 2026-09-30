;; EXPECT: :a :b :c :d
;; Catch clauses accept the package-qualified class names too.
(println (try (throw (ex-info "x" {})) (catch clojure.lang.ExceptionInfo e :a))
         (try (/ 1 0) (catch java.lang.ArithmeticException e :b))
         (try (/ 1 0) (catch java.lang.Throwable e :c))
         (try (throw (ex-info "x" {})) (catch java.lang.RuntimeException e :d)))
