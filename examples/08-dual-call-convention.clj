;; The dual call convention — all four shapes work.

;; 1. positional only — Clojure idiom unchanged
(defn add [a b] (+ a b))
(println (add 3 4))

;; 2. named-arg destructuring on the callee + trailing kv pairs at the
;;    call site (no map literal needed; trailing :keyword value pairs
;;    are auto-packaged into a kwArgs map by the compiler).
(defn area [w h & {:keys [unit] :or {unit :meters}}]
  (println "area =" (* w h) "in" unit))
(area 6 7)
(area 6 7 :unit :feet)

;; 3. The same callee accepts a trailing map literal too.
(area 6 7 {:unit :inches})

;; 4. `:as` snapshots the raw kwArgs map for forwarding to other fns
;;    that expect a map (e.g. logging, downstream defns).
(defn config [& {:keys [host port]
                 :or {host "localhost" port 8080}
                 :as opts}]
  (println "host=" host "port=" port "raw=" opts))

(config)
(config :port 9090)
(config :host "example.com" :port 443)
