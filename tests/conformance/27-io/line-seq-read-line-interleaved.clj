;; EXPECT: line-0 line-1 line-2 line-3 line-4 99997 line-99999 true
;; line-seq and read-line on one reader take each line exactly once, in
;; order, across many refills of the reader's buffer (about 1.2 MB, some
;; lines ending in CRLF): line-seq reads its first line at once and each
;; next one when the seq is read, as in Clojure, so a read-line between
;; them takes the line in between. Most lines are taken from the buffer
;; without a read (IO.cpp, readLineOf).
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(def f (str dir "/many.txt"))
(spit f (apply str (map (fn [i] (str "line-" i (if (zero? (mod i 7)) "\r\n" "\n"))) (range 100000))))
(with-open [r (reader f)]
  (let [a (read-line r)
        s (line-seq r)
        b (read-line r)
        c (second s)
        d (read-line r)
        n (count s)]
    (println a (first s) b c d n (last s)
             (= (map (fn [i] (str "line-" i)) (concat [1 3] (range 5 100000))) s))))
(sh "rm" "-rf" dir)
