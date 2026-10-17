#!/usr/bin/env protoclj
;; A Unix filter run as a command: it counts the lines of standard input
;; and prints the count, like `wc -l`. Executable (chmod +x), it ends a
;; pipeline of other commands:
;;
;;   printf 'pump-1 ok\npump-2 ALARM\npump-3 ALARM\n' \
;;     | ./13-filter-grep.clj ALARM | ./14-filter-count.clj         ; 2
(defn count-lines [n]
  (if (nil? (read-line))
    n
    (recur (inc n))))

(println (count-lines 0))
