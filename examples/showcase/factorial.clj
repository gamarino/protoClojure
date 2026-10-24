;; (factorial 1000) with exact integers, written the portable Clojure way:
;; `*'` promotes to a big integer when a product overflows 64 bits. Prints
;; the number of digits, their sum and the first 30. Runs unchanged under
;; protoclj and bb.
(defn factorial [n] (reduce *' (range 1 (inc n))))

(let [digits (str (factorial 1000))]
  (println "digits" (count digits)
           "digit-sum" (reduce + (map #(parse-long (str %)) digits))
           "head" (subs digits 0 30)))
