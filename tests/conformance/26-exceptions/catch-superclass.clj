;; EXPECT: :runtime :exception :throwable :iobe
;; A clause naming a superclass catches its subclasses.
(println (try (/ 1 0) (catch RuntimeException e :runtime))
         (try (throw (ex-info "x" {})) (catch Exception e :exception))
         (try (+ 1 "a") (catch Throwable e :throwable))
         (try (nth "abc" 7) (catch IndexOutOfBoundsException e :iobe)))
