(def counter (atom 0))

(swap! counter inc)
(swap! counter + 10)
@counter
