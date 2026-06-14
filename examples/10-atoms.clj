;; Atoms — mutable identity wrapping an immutable value, with a
;; lock-free CAS swap. The kernel primitive is protoCore's
;; setAttributeIfEqual; `swap!` retries on contention.

;; A counter you can read and increment.
(def counter (atom 0))
(println "start:" @counter)
(swap! counter inc)
(swap! counter inc)
(println "two incs:" @counter)

;; reset! / swap! / compare-and-set!.
(def state (atom :idle))
(reset! state :booting)
(println @state)
(compare-and-set! state :booting :running)
(println @state)
(println (compare-and-set! state :booting :error))   ;; false
(println @state)                                     ;; still :running

;; Atom over a map — the bookkeeping idiom.
(def stats (atom {:hits 0 :miss 0}))
(swap! stats assoc :hits 1)
(swap! stats assoc :miss 5)
(println "stats:" @stats)

;; Closure capturing an atom — the natural shape for an iterator,
;; a memoised lookup table, or a tiny FSM driven by `swap!`.
(defn make-counter [start]
  (let [a (atom start)]
    (fn [] (swap! a inc))))

(def c (make-counter 100))
(println (c) (c) (c))                                ;; 101 102 103
