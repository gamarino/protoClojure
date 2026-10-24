;; EXPECT: :ok
;; cheshire.core/parse-string: objects are maps with string keys, or keyword
;; keys with `true`, or keys through a fn; arrays are vectors; integers are
;; exact at any size; numbers with a fraction or an exponent are doubles;
;; escapes and surrogate pairs; nil for nil or a blank string; whatever
;; follows the first value is ignored, as Jackson ignores it.
(require '[cheshire.core :as json])
(def document "{\"a\":1,\"b\":[1,2.5,1e2,-0,-0.0,12345678901234567890123],\"c\":null,\"d\":{\"e\":[true,false,{}]},\"s\":\"q\\\"b\\\\s\\/n\\nt\\tu\\u00e9\\uD83D\\uDE00\"}")
(def checks
  [[:strings (= {"a" 1 "b" [1 2.5 100.0 0 -0.0 12345678901234567890123] "c" nil
                 "d" {"e" [true false {}]} "s" "q\"b\\s/n\nt\tu\u00e9\uD83D\uDE00"}
                (json/parse-string document))]
   [:keywords (= {:a 1 :b [1 2]} (json/parse-string "{\"a\":1,\"b\":[1,2]}" true))]
   [:nested-keywords (= {:a {:b [{:c 1}]}} (json/parse-string "{\"a\":{\"b\":[{\"c\":1}]}}" true))]
   [:key-fn (= {:xa 1} (json/parse-string "{\"a\":1}" (fn [k] (keyword (str "x" k)))))]
   [:false-key-fn (= {"a" 1} (json/parse-string "{\"a\":1}" false))]
   [:vector (vector? (get (json/parse-string "{\"v\":[1]}") "v"))]
   [:types (= ["7" "7.0"] [(str (json/parse-string "7")) (str (json/parse-string "7.0"))])]
   [:big (= 123456789012345678901234567890 (json/parse-string "123456789012345678901234567890"))]
   [:float-exp (= [1.5E300 100.0 -12.0] (json/parse-string "[1.5e300, 1E2, -12.0]"))]
   [:overflow (= ##Inf (json/parse-string "1e400"))]
   [:whitespace (= {"a" [] "b" {}} (json/parse-string " \n {\"a\" : [ ] ,\r\n \"b\":{}}\t "))]
   [:duplicate (= {"a" 2} (json/parse-string "{\"a\":1,\"a\":2}"))]
   [:trailing (= [1] (json/parse-string "[1] x"))]
   [:nil-input (nil? (json/parse-string nil))]
   [:blank (nil? (json/parse-string "   "))]
   [:decode (= [1] (json/decode "[1]"))]
   [:full-name (= {"a" 1} (cheshire.core/parse-string "{\"a\":1}"))]])
(def bad (map first (remove second checks)))
(println (if (empty? bad) :ok (vec bad)))
