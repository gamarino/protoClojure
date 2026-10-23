;; EXPECT: 9 9
;; gensym called by the macro does what `x#` does, for a name built by code.
(defmacro square [e]
  (let [g (gensym "sq")]
    `(let [~g ~e] (* ~g ~g))))
(def calls (atom 0))
(println (square (do (swap! calls inc) 3)) (+ (* 4 @calls) 5))
