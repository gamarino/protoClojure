;; EXPECT: user/inc 100
;; Once the program defines a name a built-in also has, syntax-quote names the
;; program's definition, user/x, as Clojure's reader resolves it in the
;; current namespace.
(defn inc [x] 100)
(defmacro call-inc [x] `(inc ~x))
(println (first (macroexpand-1 '(call-inc 1))) (call-inc 1))
