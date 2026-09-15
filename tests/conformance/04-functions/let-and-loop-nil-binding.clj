;; EXPECT: nil nil nil
(defn let-nil [] (let [a nil] a))
(defn loop-nil [] (loop [i nil] i))
(defn param-nil [p] p)
(println (let-nil) (loop-nil) (param-nil nil))
