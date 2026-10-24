;; EXPECT: :ok 29
;; Every printable value reads back equal: read-string of pr-str is the
;; identity on data (stage 12). Nested collections, keywords and symbols with
;; namespaces, strings with every escape the printer writes, integers of any
;; size, floats (shortest round-trip digits), nil and booleans.
(def values
  [nil true false 0 -1 42 9223372036854775807 -9223372036854775808
   123456789012345678901234567890 -98765432109876543210987654321
   1.5 -0.0 0.1 1.0E21 4.9E-324 (/ 1.0 3)
   "" "plain" "quote \" backslash \\ newline \n tab \t return \r ff \f bs \b"
   "UTF-8: é ñ 😀" "nul \u0000 inside"
   :k :ns/k 'sym 'ns/sym
   [1 [2 [3 []]]] '(1 (2 ()) "x") #{1 :a "b" [1 2]}
   {:a {:b [1 2 {:c #{:d}}]} "s" nil 1 2.5 [:v] {:m 1}}])
(def bad (remove (fn [v] (= v (read-string (pr-str v)))) values))
(println (if (empty? bad) :ok (vec bad)) (count values))
