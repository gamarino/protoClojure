;; EXPECT: 6
;; `~'name` puts an unqualified symbol in the expansion on purpose, the
;; Clojure idiom for an anaphoric macro that binds `it` for the caller.
(defmacro aif [test then] `(let [~'it ~test] (if ~'it ~then nil)))
(println (aif 5 (+ it 1)))
