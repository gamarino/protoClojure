;; EXPECT: [(quote x) (fn* [p1__N#] (inc p1__N#)) (clojure.core/seq (clojure.core/concat (clojure.core/list (quote clojure.core/map)) (clojure.core/list f))) [1 3] 6 :first]
;; read-string reads as code is read: quote, #(...), syntax-quote qualified
;; as the drivers qualify it, #_ discarded; only the first form is read; the
;; form can be evaluated.
(defn gensyms-out [form] (read-string (clojure.string/replace (pr-str form) #"__\d+#" "__N#")))
(println [(read-string "'x")
          (gensyms-out (read-string "#(inc %)"))
          (read-string "`(map ~f)")
          (read-string "[1 #_2 3]")
          (eval (read-string "(+ 1 2 3)"))
          (read-string ":first :second")])
