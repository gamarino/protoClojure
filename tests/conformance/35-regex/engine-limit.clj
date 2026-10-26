;; EXPECT: StackOverflowError StackOverflowError true
;; A match past the engine's limits is a StackOverflowError, never a silent
;; nil (M48): `(a|b)*` takes 32 bytes of the 64 MiB JIT stack per character,
;; so four million characters exceed it. Java's recursive engine overflows
;; its thread stack on such a match too. The error is raised in the caller
;; and leaves the engine usable.
(defn twice [s] (str s s))
(def huge (loop [s "a"] (if (< (count s) 4000000) (recur (twice s)) s)))
(println (try (re-find #"(a|b)*" huge) (catch StackOverflowError e "StackOverflowError"))
         (try (count (re-seq #"(a|b)*" huge)) (catch StackOverflowError e "StackOverflowError"))
         (= "aaa" (re-find #"a+" "xaaa")))
