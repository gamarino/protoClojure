;; EXPECT: :p 1 10
;; The positional parameters of a keyword-argument fn may receive keywords:
;; the callee arity, not the first keyword in the call, decides where the
;; key/value pairs start.
(defn f [a b & {:keys [x]}] (println a b x))
(f :p 1 :x 10)
