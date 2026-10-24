;; EXPECT: :ok
;; cheshire.core/generate-string: keywords keep their namespace, other keys
;; are their str; nil is null; lists, vectors, sets and lazy seqs are arrays;
;; doubles print as Java prints them; NaN and infinities are quoted, as
;; Jackson writes them; control characters are \uXXXX in upper case; with
;; :escape-non-ascii every non-ASCII character is \uXXXX (a surrogate pair
;; beyond the BMP); :key-fn applies to keyword keys.
(require '[cheshire.core :as json])
(def checks
  [[:scalars (= "[1,2.5,null,true,false,\"s\"]" (json/generate-string [1 2.5 nil true false "s"]))]
   [:keyword-ns (= "{\"c/d\":\"a/kw\"}" (json/generate-string {:c/d :a/kw}))]
   [:key-str (= "{\"3\":4}" (json/generate-string {3 4}))]
   [:sym (= "\"sym\"" (json/generate-string 'sym))]
   [:seqs (= "[[1,2],[2,3],[1]]" (json/generate-string [(list 1 2) (map inc [1 2]) #{1}]))]
   [:doubles (= "[1.0E21,0.1,100.0,1.0E-5,-0.0]" (json/generate-string [1e21 0.1 100.0 1e-5 -0.0]))]
   [:big (= "12345678901234567890123" (json/generate-string 12345678901234567890123))]
   [:nan (= "[\"NaN\",\"Infinity\",\"-Infinity\"]" (json/generate-string [##NaN ##Inf ##-Inf]))]
   [:escapes (= "\"q\\\"b\\\\s/\\n\\t\\u0001\\u001F\u00e9\"" (json/generate-string "q\"b\\s/\n\t\u0001\u001f\u00e9"))]
   [:non-ascii (= "\"\\u00E9\\uD83D\\uDE00\"" (json/generate-string "\u00e9\uD83D\uDE00" {:escape-non-ascii true}))]
   [:key-fn (= "{\"K:b\":2}" (json/generate-string {:b 2} {:key-fn (fn [k] (str "K" k))}))]
   [:key-fn-strings (= "{\"a\":1}" (json/generate-string {"a" 1} {:key-fn (fn [k] "never")}))]
   [:nil-key (= "{\"\":1}" (json/generate-string {nil 1}))]
   [:empty (= ["[]" "{}" "null"] [(json/generate-string []) (json/generate-string {}) (json/generate-string nil)])]
   [:encode (= "[1]" (json/encode [1]))]])
(def bad (map first (remove second checks)))
(println (if (empty? bad) :ok (vec bad)))
