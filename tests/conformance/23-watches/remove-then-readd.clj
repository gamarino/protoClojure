;; EXPECT: :b:a
;; Watches fire in the order they were added. remove-watch drops a watch
;; without disturbing the others, and a watch added again goes last. The
;; JVM leaves watch order unspecified; this order is protoClojure-defined.
(def a (atom 0))
(def log (atom ""))
(add-watch a :a (fn [k r o n] (swap! log (fn [s] (str s k)))))
(add-watch a :b (fn [k r o n] (swap! log (fn [s] (str s k)))))
(remove-watch a :a)
(add-watch a :a (fn [k r o n] (swap! log (fn [s] (str s k)))))
(reset! a 1)
(println @log)
