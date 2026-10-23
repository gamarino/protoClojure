;; EXPECT: :ran nil :else :then
;; when-not runs its body when the test is false; if-not swaps the branches.
(println (when-not false :ran) (when-not true :never) (if-not true :then :else)
         (if-not nil :then :else))
