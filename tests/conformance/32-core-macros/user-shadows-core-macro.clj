;; EXPECT: :mine 2
;; A program may define its own `when`: its later forms use it. A template
;; in a core macro still calls core's (clojure.core/when).
(defmacro when [t & body] :mine)
(println (when false 1) (when-let [x 2] x))
