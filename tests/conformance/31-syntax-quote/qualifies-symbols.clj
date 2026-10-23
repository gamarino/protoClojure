;; EXPECT: (clojure.core/str user/x) (if a b) (clojure.core/let [y 1] y) (catch Exception e) &
;; Syntax-quote qualifies symbols as Clojure's reader does: a built-in becomes
;; clojure.core/x, any other name user/x (the program's namespace). Special
;; forms, `&`, exception class names and already qualified symbols stay.
(println `(str x) `(if ~'a ~'b) `(let [~'y 1] ~'y) `(catch Exception ~'e) `&)
