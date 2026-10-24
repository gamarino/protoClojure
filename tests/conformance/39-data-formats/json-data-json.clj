;; EXPECT: :ok
;; clojure.data.json: read-str with :key-fn and :value-fn (a value-fn that
;; answers itself drops the entry), :eof-error? / :eof-value; write-str
;; escapes non-ASCII as \uxxxx in lower case and `/` as `\/` by default,
;; writes a keyword's name without its namespace.
(require '[clojure.data.json :as json])
(defn drop-b [k v] (if (= k :b) drop-b (inc v)))
(def checks
  [[:read (= {"a" 1 "b" [1 2.5]} (json/read-str "{\"a\":1,\"b\":[1,2.5]}"))]
   [:key-fn (= {:a 1} (json/read-str "{\"a\":1}" :key-fn keyword))]
   [:value-fn (= {:a 2} (json/read-str "{\"a\":1,\"b\":2}" :key-fn keyword :value-fn drop-b))]
   [:eof-value (= :none (json/read-str "" :eof-error? false :eof-value :none))]
   [:write (= "{\"d\":\"x\\/y\\u00e9\\u2028\"}" (json/write-str {:c/d "x/y\u00e9\u2028"}))]
   [:write-plain (= "{\"a\":\"x/y\u00e9\"}" (json/write-str {:a "x/y\u00e9"} :escape-slash false :escape-unicode false))]
   [:js-separators (= "\"\\u2028\"" (json/write-str "\u2028" :escape-unicode false))]
   [:key-fn-write (= "{\"A\":1}" (json/write-str {:a 1} :key-fn (fn [k] (clojure.string/upper-case (name k)))))]
   [:control (= "\"\\u0001\\b\"" (json/write-str "\u0001\b"))]
   [:full-name (= [1] (clojure.data.json/read-str "[1]"))]])
(def bad (map first (remove second checks)))
(println (if (empty? bad) :ok (vec bad)))
