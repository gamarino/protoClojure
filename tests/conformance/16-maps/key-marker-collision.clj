;; EXPECT: nil nil nil false 2
;; Keys of different types are never confused, whatever they are built from.
;; The vectors below spell out the marker tuples the old interned
;; canonical-key layer built for a double, a map and a big integer; each key
;; type now carries its own hash salt instead. (The bit pattern of 1.0 is
;; 0x3FF0000000000000: 1072693248 and 0.)
(println (get {1.0 :d} [:double 1072693248 0])
         (get {[:double 1072693248 0] :v} 1.0)
         (get {{:a 1} :m} [:map :a 1])
         (= {1.0 :x} {[:double 1072693248 0] :x})
         (count (hash-map 1.0 :d [:double 1072693248 0] :v)))
