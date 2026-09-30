;; EXPECT: :unsupported
(println (try (nth {:a 1} 0) (catch UnsupportedOperationException e :unsupported)))
