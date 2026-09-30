;; EXPECT: :vector :string
;; StringIndexOutOfBoundsException is an IndexOutOfBoundsException.
(println (try (nth [1 2] 5) (catch IndexOutOfBoundsException e :vector))
         (try (nth "ab" 5) (catch StringIndexOutOfBoundsException e :string)))
