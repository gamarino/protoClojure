;; EXPECT: true 8 1
;; A keyword first used on several threads at once is one value: every pmap
;; worker and the main thread obtain the same keyword, which is = to the
;; literal and finds its map entry.
(def ks (pmap (fn [i] :first-used-on-worker-threads) (list 1 2 3 4 5 6 7 8)))
(defn all-equal? [xs k]
  (if (empty? xs) true (if (= (first xs) k) (all-equal? (rest xs) k) false)))
(println (all-equal? ks :first-used-on-worker-threads)
         (count ks)
         (get (hash-map :first-used-on-worker-threads 1) (first ks)))
