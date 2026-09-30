;; EXPECT: :error :still-running
;; StackOverflowError is an Error, not an Exception: a catch of Exception does
;; not see it, a catch of Error or Throwable does, and the program continues.
(defn forever [n] (+ 1 (forever n)))
(println (try (try (forever 0) (catch Exception e :exception))
              (catch Error e :error))
         (try (forever 0) (catch StackOverflowError e :still-running)))
