;; EXPECT: 0 32 (1 3 5) 1
;; for is lazy: over a range it realises a chunk of 32 when first read,
;; over a list one element; it works over endless seqs.
(def n (atom 0))
(def s (for [x (range 100)] (do (swap! n inc) x)))
(def before @n)
(first s)
(def m (atom 0))
(first (for [x '(1 2 3)] (do (swap! m inc) x)))
(println before @n (take 3 (for [x (range) :when (odd? x)] x)) @m)
