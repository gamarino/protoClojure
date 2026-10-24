;; EXPECT: ERROR b
;; Tutorial chapter 14: the grep over line-seq (over a reader here, *in*
;; in the chapter, which tests/cli/lazy-grep-under-heap-limit.sh runs).
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/in.txt"))
(spit f "a\nERROR b\nc\n")
(with-open [*in* (reader f)]
  (doseq [line (line-seq *in*)]
    (when (re-find #"ERROR" line)
      (println line))))
(sh "rm" "-rf" dir)
