;; EXPECT: hello world !
(defn greet [prefix name & {:keys [punct]}]
  (println prefix name punct))
(greet "hello" "world" {:punct "!"})
