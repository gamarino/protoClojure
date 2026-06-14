;; EXPECT: true :running false :running
(def state (atom :idle))
(println (compare-and-set! state :idle :running)
         @state
         (compare-and-set! state :idle :stopped)
         @state)
