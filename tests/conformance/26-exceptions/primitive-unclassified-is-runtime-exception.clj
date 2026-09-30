;; EXPECT: inc: expects 1 arg
;; A runtime error whose message names no class is a RuntimeException that
;; carries the whole message.
(println (try (inc 1 2) (catch RuntimeException e (ex-message e))))
