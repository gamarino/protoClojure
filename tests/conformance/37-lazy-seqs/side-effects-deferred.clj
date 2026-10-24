;; EXPECT: [] [1] [1]
;; map over a list runs f only when an element is read, one at a time, and
;; reading it again does not run f again (the result is cached).
(def log (atom []))
(def s (map (fn [x] (swap! log conj x) x) '(1 2 3)))
(def before @log)
(first s)
(def after-first @log)
(first s)
(println before after-first @log)
