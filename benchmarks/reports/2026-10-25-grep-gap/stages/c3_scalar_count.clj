;; Stage c3: scalar accumulator summing (count line): c_vector without the
;; vector and its destructuring.
(require '[clojure.java.io :as io])
(println "chars" (reduce (fn [t line] (+ t (count line))) 0 (line-seq (io/reader *in*))))
