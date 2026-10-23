;; EXPECT: before body after => :result
;; A `with-` macro wraps its body in set-up and tear-down code that runs even
;; when the body throws, the shape of with-open and with-redefs.
(def log (atom []))
(defmacro with-logging [label & body]
  `(do (swap! log conj (str ~label " before"))
       (try ~@body
            (finally (swap! log conj (str ~label " after"))))))
(def r (with-logging "x" (swap! log conj "x body") :result))
(try (with-logging "y" (throw (ex-info "boom" {}))) (catch Exception e nil))
(println (subs (nth @log 0) 2) (subs (nth @log 1) 2) (subs (nth @log 2) 2) "=>" r)
