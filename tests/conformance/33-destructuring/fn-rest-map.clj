;; EXPECT: {:a 1, :b 2} nil 3 4
;; `& {...}` with :as alone, or with :strs, destructures the rest arguments
;; as key/value pairs; `& {:keys ...}` keeps the keyword-argument call
;; convention.
(defn opts [& {:as o}] o)
(defn s [& {:strs [n]}] n)
(defn k [& {:keys [n] :or {n 4}}] n)
(println (opts :a 1 :b 2) (opts) (s "n" 3) (k))
