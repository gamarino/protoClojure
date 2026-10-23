;; EXPECT-ERROR: :keys must be followed by a vector
;; :keys takes a vector of names.
(let [{:keys a} {}] a)
