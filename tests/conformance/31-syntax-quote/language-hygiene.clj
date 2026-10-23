;; EXPECT: :mine
;; LANGUAGE.md §6.1, "Hygiene": my-or with an auto-gensym does not capture
;; the caller's local named `or`.
(defmacro my-or
  ([] nil)
  ([x] x)
  ([x & next] `(let [or# ~x] (if or# or# (my-or ~@next)))))
(println (let [or :mine] (my-or false or)))
