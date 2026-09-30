;; EXPECT-ERROR: runtime error: ExceptionInfo: boom {:a 1}
;; An uncaught exception stops the script with exit status 1 and the
;; runtime-error report, which names the class.
(throw (ex-info "boom" {:a 1}))
