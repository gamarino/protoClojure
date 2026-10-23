;; EXPECT: (clojure.core/deref a) 5
;; @x reads as (clojure.core/deref x), as in Clojure, and calls the built-in
;; deref even where a local is named deref.
(def a (atom 5))
(defn f [deref] @a)
(println '@a (f 0))
