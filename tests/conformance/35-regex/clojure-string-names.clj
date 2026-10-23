;; EXPECT: ["a" "b"] "A-B" true true "x"
;; clojure.string/x and clojure.set/x name the built-in x without a require
;; (no namespaces yet, D28); they are never captured by a local.
(let [split 1 union 2]
  (prn (clojure.string/split "a b" #" ")
       (clojure.string/upper-case (clojure.string/join "-" ["a" "b"]))
       (clojure.string/blank? "  ")
       (= (clojure.set/union #{1} #{2}) #{1 2})
       (apply clojure.string/trim ["  x "])))
