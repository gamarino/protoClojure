;; EXPECT: 300000 2 300000 no-crash
;; Matching a long subject does not recurse per character for a pattern
;; without back-references. One with back-references either matches (libc++
;; backtracks on the heap) or, where the engine recurses on the native stack
;; and the subject could exhaust it (libstdc++, MSVC), raises
;; StackOverflowError instead of crashing (D45). Either answer is "no-crash".
(defn twice [s] (str s s))
(def big (loop [s "aaa" n 0] (if (< (count s) 300000) (recur (twice s) (inc n)) (subs s 0 300000))))
(println (count (re-find #".*" big))
         (count (re-find #"(a|b)*" big))
         (count (first (re-find #"(a|b)*" big)))
         (try (when (= 300000 (count (first (re-find #"(a)\1*" big)))) "no-crash")
              (catch StackOverflowError e "no-crash")))
