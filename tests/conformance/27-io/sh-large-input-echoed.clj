;; EXPECT: 2097152
;; 2 MiB through `cat` comes back whole (input and output are pumped together).
(defn big []
  (loop [s "y" i 0]
    (if (< i 21) (recur (str s s) (+ i 1)) s)))
(println (count (:out (sh "cat" :in (big)))))
