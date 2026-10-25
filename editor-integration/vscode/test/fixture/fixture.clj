;; protoClojure fixture for the VS Code extension tests: line numbers matter.
(defn twice [n]
  (let [doubled (* n 2)]
    (identity doubled)))
(defn risky [n]
  (throw (ex-info (str "boom " n) {:n n})))
(println "e2e-before")
(println (twice 21))
(def r
  (try (risky 7)
    (catch Exception e (str "caught " (ex-message e)))))
(println r)
(println "e2e-after")
