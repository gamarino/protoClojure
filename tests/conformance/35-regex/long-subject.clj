;; EXPECT: 300000 2 300000 StackOverflowError
;; Matching a long subject does not recurse per character for a pattern
;; without back-references; one with back-references that could exhaust the
;; native stack raises StackOverflowError instead of crashing (D45).
(defn twice [s] (str s s))
(def big (loop [s "aaa" n 0] (if (< (count s) 300000) (recur (twice s) (inc n)) (subs s 0 300000))))
(println (count (re-find #".*" big))
         (count (re-find #"(a|b)*" big))
         (count (first (re-find #"(a|b)*" big)))
         (try (re-find #"(a)\1*" big) (catch StackOverflowError e "StackOverflowError")))
