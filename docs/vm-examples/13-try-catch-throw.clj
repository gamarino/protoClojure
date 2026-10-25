(defn checked-div [a b]
  (if (= b 0)
    (throw (ex-info "division by zero" {:a a}))
    (quot a b)))

(defn safe-div [a b]
  (try
    (checked-div a b)
    (catch clojure.lang.ExceptionInfo e
      (ex-data e))
    (finally
      (str "done"))))

(safe-div 10 0)
