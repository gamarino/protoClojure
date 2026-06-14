;; EXPECT: hello world !
(defn make-greeter [prefix]
  (fn [name & {:keys [punct]}]
    (println prefix name punct)))
((make-greeter "hello") "world" {:punct "!"})
