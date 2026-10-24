;; The same factorial with plain `*`. Here the runtimes differ, on purpose:
;; Clojure's `*` throws on 64-bit overflow (bb: "ArithmeticException: long
;; overflow"; use `*'` or `bigint`, as factorial.clj does), while
;; protoClojure's `*` promotes to an exact big integer by itself (D14).
(defn factorial [n] (reduce * (range 1 (inc n))))

(println (try (count (str (factorial 1000)))
              (catch ArithmeticException e (str "ArithmeticException: " (ex-message e)))))
