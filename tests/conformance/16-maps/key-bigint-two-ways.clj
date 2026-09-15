;; EXPECT: :big :big :neg nil nil 1 :huge nil
;; A big integer key is found with the same value computed another way, of
;; any size; a negative value, and an equal double, are different keys.
(def e20 (* 10000000000 10000000000))
(def t50 1125899906842624)
(def t100 (* t50 t50))
(def t200 (* t100 t100))
(println (get {100000000000000000000 :big} e20)
         (get {e20 :big} 100000000000000000000)
         (get {(- 0 100000000000000000000) :neg} (- 0 e20))
         (get {100000000000000000000 :big} (- 0 e20))
         (get {100000000000000000000 :big} 1.0e20)
         (count (hash-map e20 1 100000000000000000000 2))
         (get {t200 :huge} (* (* t50 t50) (* t50 t50)))
         (get {t200 :huge} (+ t200 1)))
