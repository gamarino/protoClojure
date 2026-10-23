;; EXPECT-ERROR: Unsupported binding form: :a
;; A keyword is not a binding form.
(let [:a 1] 2)
