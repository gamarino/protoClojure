;; EXPECT: :k s 5 nil () [] (user/a)
;; Self-evaluating literals stay themselves; empty collections stay empty.
(println `:k `"s" `5 `nil `() `[] `(a ~@nil))
