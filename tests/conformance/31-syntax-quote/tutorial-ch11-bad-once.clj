;; EXPECT-ERROR: Can't let qualified name: user/x
;; docs/tutorial/11-macros.md §11.3.2: a plain name bound in a template is
;; qualified and refused.
(defmacro bad-once [expr]
  `(let [x ~expr] (* x x)))

(let [x 10]
  (bad-once (+ 1 2)))
