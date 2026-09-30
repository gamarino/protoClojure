;; EXPECT: 101
;; An exception thrown several calls deep unwinds those frames; values the
;; enclosing expression pushed before the try are kept.
(defn down [n] (if (= n 0) (throw (ex-info "bottom" {:v 100})) (+ 1 (down (- n 1)))))
(println (+ 1 (try (down 50) (catch ExceptionInfo e (:v (ex-data e))))))
