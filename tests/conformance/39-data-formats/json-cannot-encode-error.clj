;; EXPECT-ERROR: Cannot JSON encode object of class: atom
(cheshire.core/generate-string {:a (atom 1)})
