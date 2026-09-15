;; EXPECT: 42 façade café
;; Symbols spelled with non-ASCII letters name globals, fns, parameters and
;; locals, and quote to their spelling.
(def café 21)
(defn año [ñ] (let [dos 2 résultat (* ñ dos)] résultat))
(println (año café) (quote façade) (str (quote café)))
