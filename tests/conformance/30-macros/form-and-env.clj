;; EXPECT: (show-call 1 2) nil [a b]
;; A macro receives two hidden arguments, as in Clojure: &form, the whole
;; call as written, and &env, the locals in scope (nil at the top level).
(defmacro show-call [& _] (list 'quote &form))
(defmacro env-nil? [] (nil? &env))
(defmacro locals [] (list 'quote (vec (sort-names (keys &env)))))
(defn sort-names [syms] (if (= (str (first syms)) "a") syms (reverse syms)))
(defn f [a] (let [b 1] (locals)))
(println (show-call 1 2) (if (env-nil?) nil :not-nil) (f 0))
