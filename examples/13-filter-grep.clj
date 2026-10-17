#!/usr/bin/env protoclj
;; A Unix filter run as a command: it prints the lines of standard input
;; that contain PATTERN, like `grep -F`. The first line is an interpreter
;; line, a `#!` comment as in Clojure, so once the file is executable
;; (chmod +x 13-filter-grep.clj) it runs as a command and chains with
;; other commands:
;;
;;   printf 'pump-1 ok\npump-2 ALARM\n' | ./13-filter-grep.clj ALARM
;;
;; Exit status, as grep's: 0 when a line matched, 1 when none did, 2 on a
;; usage error.
(when (nil? *command-line-args*)
  (println "usage: 13-filter-grep.clj PATTERN")
  (exit 2))

(def pattern (first *command-line-args*))

(defn scan [found]
  (let [line (read-line)]
    (cond
      (nil? line) found
      (includes? line pattern) (do (println line) (recur true))
      :else (recur found))))

(exit (if (scan false) 0 1))
