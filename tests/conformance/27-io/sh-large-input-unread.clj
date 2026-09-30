;; EXPECT: 0 2097152 alive
;; 2 MiB fed to a program that never reads its input (`true`) neither kills
;; the runtime with SIGPIPE nor hangs it.
(defn big []
  (loop [s "x" i 0]
    (if (< i 21) (recur (str s s) (+ i 1)) s)))
(def input (big))
(def r (sh "true" :in input))
(println (:exit r) (count input) "alive")
