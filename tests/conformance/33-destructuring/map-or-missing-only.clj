;; EXPECT: 5 false nil
;; :or supplies a default only when the key is missing, as in Clojure: a
;; present false or nil value is kept (unlike `& {:keys ...}`, D16).
(let [{:keys [a b c] :or {a 5 b 6 c 7}} {:b false :c nil}]
  (println a b c))
