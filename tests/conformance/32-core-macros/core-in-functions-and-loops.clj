;; EXPECT: 12 :odd
;; The core macros compose inside functions and loops.
(defn sum-evens [xs]
  (let [acc (atom 0)]
    (doseq [x xs] (when (even? x) (swap! acc + x)))
    @acc))
(defn parity [n] (case (odd? n) true :odd false :even))
(println (sum-evens [1 2 3 4 6]) (parity 3))
