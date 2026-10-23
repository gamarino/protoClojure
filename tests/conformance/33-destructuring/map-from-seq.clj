;; EXPECT: 7 {:b 2} 3
;; A map binding form over a seq of key/value pairs destructures the map they
;; make, and over a one-element seq its element, as Clojure 1.11 does.
(let [{:keys [a]} '(:a 7)
      {:as m} '(:b 2)
      {:keys [c]} (list {:c 3})]
  (println a m c))
