(defn sign-word [n]
  (if (< n 0) :negative :non-negative))

(defn inc-when [x]
  (when x (inc x)))

(defn classify [n]
  (cond (< n 0) :negative
        (= n 0) :zero
        :else   :positive))
