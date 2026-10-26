;; EXPECT: 300000 2 300000 300000
;; Matching a long subject: the engine (PCRE2's JIT) backtracks on a stack
;; of its own, which grows up to 64 MiB a thread (M48), so these match on
;; every platform; none recurses on the native stack.
(defn twice [s] (str s s))
(def big (loop [s "aaa" n 0] (if (< (count s) 300000) (recur (twice s) (inc n)) (subs s 0 300000))))
(println (count (re-find #".*" big))
         (count (re-find #"(a|b)*" big))
         (count (first (re-find #"(a|b)*" big)))
         (count (first (re-find #"(a)\1*" big))))
