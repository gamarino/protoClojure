;; EXPECT-ERROR: Can't let qualified name: user/v
;; A let binding written plainly in a template comes out qualified and is
;; rejected, which is what keeps such a macro from capturing a caller's v.
(defmacro bad-or [a b] `(let [v ~a] (if v v ~b)))
(bad-or false 1)
