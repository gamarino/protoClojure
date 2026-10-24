;; EXPECT: (0 1 1 2 3 5 8 13 21 34)|(1 9 25 49 81)|ERROR two
;; README.md, "A flavour of the language": lazy sequences (the grep reads a
;; file here instead of the standard input).
(defn fib-seq
  ([] (fib-seq 0 1))
  ([a b] (lazy-seq (cons a (fib-seq b (+ a b))))))
(print (take 10 (fib-seq)))
(print "|")
(print (->> (range) (filter odd?) (map (fn [x] (* x x))) (take 5)))
(print "|")
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/log.txt"))
(spit f "ok one\nERROR two\nok three\n")
(with-open [r (reader f)]
  (doseq [line (line-seq r)]
    (when (re-find #"ERROR" line) (println line))))
(sh "rm" "-rf" dir)
