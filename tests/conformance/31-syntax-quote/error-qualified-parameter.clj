;; EXPECT-ERROR: Can't use qualified name as parameter: user/x
;; A parameter written plainly in a template comes out qualified, as in
;; Clojure, and is rejected with Clojure's message; write x# or ~'x.
(defmacro make-identity [] `(fn [x] x))
(make-identity)
