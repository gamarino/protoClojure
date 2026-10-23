;; EXPECT: 1 2 3
;; :keys accepts keywords and qualified names; :ns/keys qualifies its names.
(let [{:keys [a :b] :user/keys [c]} {:a 1 :b 2 :user/c 3}]
  (println a b c))
