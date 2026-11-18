;; EXPECT: [:cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :cce :thrown] [:not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type :not-a-type]
;; Whether `throw` accepts a value, and whether a catch clause accepts it as a
;; type, is decided by the value's type, for every kind of value: numbers
;; (small, float, big), strings, keywords, symbols, collections, fns, nil,
;; booleans and atoms are neither; an exception is throwable.
(def values [42 2.5 (reduce * (range 1 30)) "s" :k 'sym [1] '(1) {:a 1} #{1}
             inc (fn [] 1) nil true (atom 1)])
(defn thrown [v]
  (try (throw v) (catch ClassCastException e :cce) (catch Object e :thrown)))
(defn as-type [t]
  (try (try (/ 1 0) (catch t e :matched))
       (catch IllegalArgumentException e :not-a-type)))
(println (mapv thrown (conj values (ex-info "x" {}))) (mapv as-type values))
