;; EXPECT-ERROR: let: not supported at top level
;; `let` is only compiled inside a fn body; the error names the restriction.
(let [a 1] (println a))
