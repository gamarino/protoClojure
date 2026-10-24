;; EXPECT: :caught (1 2) 2
;; A body that raises leaves the lazy seq unrealised; the next read runs it
;; again, as in Clojure.
(def calls (atom 0))
(def s (lazy-seq (swap! calls inc) (if (= @calls 1) (throw (ex-info "boom" {})) [1 2])))
(def r (try (first s) (catch Exception e :caught)))
(def v (doall s))
(println r v @calls)
