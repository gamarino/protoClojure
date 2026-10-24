;; EXPECT: true
;; {:pretty true} writes Jackson's default pretty printer layout: two spaces
;; per enclosing object, `"k" : v`, arrays on one line as `[ 1, 2 ]`, empty
;; collections as `[ ]` and `{ }`.
(require '[cheshire.core :as json])
(println (= (str "{\n  \"a\" : {\n    \"c\" : [ ]\n  },\n  \"d\" : [ {\n    \"e\" : 1\n  }, { }, [ 1, [ 2 ] ] ]\n}")
            (json/generate-string {:a {:c []} :d [{:e 1} {} [1 [2]]]} {:pretty true})))
