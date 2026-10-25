(defn guarded [f T mod]
  (try
    (f)
    (catch ArithmeticException e :arithmetic)
    (catch T e :local-type)
    (catch mod/Failure e :member-type)
    (catch app.errors/Timeout e :global-type)))

(guarded (fn [] 42) nil nil)
