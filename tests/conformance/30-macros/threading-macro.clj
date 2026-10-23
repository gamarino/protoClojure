;; EXPECT: 16 (inc (* (inc 3) 3))
;; A threading macro written by hand: each step receives the previous value
;; as its first argument. A recursive expansion, one step at a time.
(defmacro thread-first
  ([x] x)
  ([x form & more]
   (let [step (if (seq? form)
                (cons (first form) (cons x (rest form)))
                (list form x))]
     (cons 'thread-first (cons step more)))))
(println (thread-first 3 inc (* 4)) (macroexpand '(thread-first 3 inc (* 3) inc)))
