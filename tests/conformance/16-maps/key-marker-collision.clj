;; EXPECT: nil nil nil false 2
;; A double, a big integer or a map used as a key is matched internally
;; through a tuple headed by a private marker. A user vector spelled like that
;; tuple, with a keyword of the same name, is a different key. (The bit
;; pattern of 1.0 is 0x3FF0000000000000: 1072693248 and 0.)
(println (get {1.0 :d} [:double 1072693248 0])
         (get {[:double 1072693248 0] :v} 1.0)
         (get {{:a 1} :m} [:map :a 1])
         (= {1.0 :x} {[:double 1072693248 0] :x})
         (count (hash-map 1.0 :d [:double 1072693248 0] :v)))
