;; EXPECT-ERROR: with-open only allows Symbols in bindings
(with-open [1 (reader "x")] 1)
