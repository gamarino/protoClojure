;; EXPECT: 2 10 (1 2)
;; A syntax-quoted symbol is never captured by a caller's local: `inc` in the
;; template is clojure.core/inc, `helper` is user/helper (the global), even
;; where the caller binds locals of those names. Redefining a built-in with
;; `def` does not change what clojure.core/x names.
(defmacro my-inc [x] `(inc ~x))
(def helper 10)
(defmacro get-helper [] `helper)
(def concat :not-the-built-in)
(println (let [inc dec] (my-inc 1))
         (let [helper 1] (get-helper))
         `(~@[1 2]))
