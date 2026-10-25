(defn sum-pair [[a b]]
  (+ a b))

(defn area [{:keys [w h]}]
  (* w h))

(defn connect [host & {:keys [port] :or {port 80}}]
  [host port])

(sum-pair [1 2])
(area {:w 3 :h 4})
(connect "example.org" :port 8080)
