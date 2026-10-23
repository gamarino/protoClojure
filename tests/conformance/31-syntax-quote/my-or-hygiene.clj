;; EXPECT: :third 5 nil clojure.core/let
;; The classic hygienic macro: `my-or` evaluates each form once and names the
;; value with an auto-gensym, so it cannot capture a caller's local, whatever
;; its name; the recursive call names user/my-or.
(defmacro my-or
  ([] nil)
  ([x] x)
  ([x & next]
   `(let [or# ~x]
      (if or# or# (my-or ~@next)))))
(def calls (atom 0))
(defn tick [v] (swap! calls inc) v)
(let [or :caller-local]
  (println (my-or false nil :third)
           (let [v 5] (my-or false v))
           (my-or)
           (first (macroexpand-1 '(my-or a b)))))
