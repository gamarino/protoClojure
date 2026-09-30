;; EXPECT: :file-not-found 2 true
;; A missing file throws FileNotFoundException carrying {:type :file-not-found :errno 2}.
(def r (try (slurp "/nonexistent-dir-protoclj/missing.txt")
            (catch FileNotFoundException e (ex-data e))))
(println (:type r) (:errno r) (map? r))
