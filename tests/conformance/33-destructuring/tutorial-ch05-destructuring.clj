;; EXPECT: 6 (3 4 5) [10 20] alice 30 alice 30 hi, alice hola, bob alice lives in BA alice is 30
;; docs/tutorial/05-functions-and-closures.md §5.6 and §5.10: destructuring.
(defn first-two [[a b]] [a b])
(defn greet [{:keys [name greeting] :or {greeting "hi"}}] (str greeting ", " name))
(println
  (let [[a b c] [1 2 3]] (+ a b c))
  (let [[a b & rest] [1 2 3 4 5]] rest)
  (first-two [10 20 30])
  (let [{name :name, age :age} {:name "alice" :age 30}] (str name " " age))
  (let [{:keys [name age]} {:name "alice" :age 30}] (str name " " age))
  (greet {:name "alice"})
  (greet {:name "bob" :greeting "hola"})
  (let [{:keys [name] {:keys [city zip]} :address} {:name "alice" :address {:city "BA" :zip 1428}}]
    (str name " lives in " city))
  (let [{:keys [name age]} {:name "alice" :age 30}] (str name " is " age)))
