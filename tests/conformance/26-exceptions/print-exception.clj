;; EXPECT: #error {:type ExceptionInfo, :message "boom", :data {:a 1}}
;; An exception prints as a one-line #error map (a simplification of JVM
;; Clojure's multi-line #error form, which also carries :via and :trace).
(println (ex-info "boom" {:a 1}))
