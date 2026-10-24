;; EXPECT: :ok
;; clojure.edn/read-string reads data: the result equals the literal, nil
;; for nil or an empty string, the :eof option for a string with no form.
(require '[clojure.edn :as edn])
(def checks
  [[:map (= {:a [1 2 #{3}] :b "s\n" 'c 1.5 'd nil :e 123456789012345678901234567890}
           (edn/read-string "{:a [1 2 #{3}] :b \"s\\n\" c 1.5 d nil :e 123456789012345678901234567890}"))]
   [:nested (= [{:k '(1 2)} #{} [] ()] (edn/read-string "[{:k (1 2)} #{} [] ()]"))]
   [:discard (= [1 3] (edn/read-string "[1 #_ 2 3]"))]
   [:comment (= 7 (edn/read-string "; leading comment\n 7"))]
   [:nil-input (nil? (edn/read-string nil))]
   [:empty (nil? (edn/read-string ""))]
   [:eof-option (= :done (edn/read-string {:eof :done} "   "))]
   [:first-only (= :a (edn/read-string ":a :b"))]
   [:symbolic (= ##Inf (edn/read-string "##Inf"))]
   [:full-name (= :edn (clojure.edn/read-string ":edn"))]])
(def bad (map first (remove second checks)))
(println (if (empty? bad) :ok (vec bad)))
