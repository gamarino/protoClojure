;; EXPECT-ERROR: Unsupported binding form, only :as can follow & parameter
;; Only :as may follow the rest binding, as in Clojure.
(let [[a & b c] [1 2 3]] a)
