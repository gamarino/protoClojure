# 15. Scripting

This chapter is for the twenty-line script: read standard input or a file,
pick lines with a regular expression, parse some JSON, count things, print
a result. It is what people use Babashka for, and the same scripts run
under protoclj, most of them unchanged. Everything here runs today on the
`feature/macros` branch; `docs/LANGUAGE.md` is the reference, and the
differences from Clojure are the D entries in `docs/STATUS.md`.

Run a script with `protoclj script.clj`, give it input with a pipe
(`protoclj script.clj < data.txt`), and read its arguments from
`*command-line-args*`. A `#!/usr/bin/env protoclj` first line makes it a
command (chapter 14).

## 15.1 Standard input, one line at a time

`(line-seq *in*)` is a lazy sequence of the lines of standard input: each
line is read when the sequence reaches it, so a script that walks the lines
without keeping them reads a stream of any size in constant memory.

<!-- not run: reads standard input (checked by hand: printf 'a\nbb\n' | protoclj lines.clj prints "2 lines, 3 characters") -->
```clojure
(let [lines (line-seq *in*)]
  (println (count lines) "lines,"
           (reduce + (map count lines)) "characters"))
```

That one holds the head: `lines` is read twice, so every line stays in
memory until the second walk ends. Walk once when the input is large:

```clojure
(defn stats [lines]
  (reduce (fn [[n chars] line] [(inc n) (+ chars (count line))]) [0 0] lines))

(let [[n chars] (stats (line-seq *in*))]
  (println n "lines," chars "characters"))
```

`(require '[clojure.java.io :as io])` and `(line-seq (io/reader *in*))`
also work, which is how a Clojure or Babashka script usually spells it;
in protoclj `reader` answers `*in*` unchanged.

## 15.2 Regular expressions

`#"..."` is a pattern, written with Java's syntax (protoClojure runs it on
the C++ standard library's ECMAScript engine; D45 lists what differs).
`re-find` answers the first match, or a vector of the match and its groups
when the pattern has groups; `re-seq` answers every match, lazily;
`re-matches` requires the whole string to match.

```clojure
(re-find #"\d+" "order 66 of 70")                 ;; => "66"
(re-find #"(\w+)@(\w+)\.org" "mail ada@lovelace.org")
;; => ["ada@lovelace.org" "ada" "lovelace"]
(re-seq #"[a-z]+" "one, two; three")             ;; => ("one" "two" "three")
(re-matches #"\d{4}-\d{2}" "2026-10")            ;; => "2026-10"
(clojure.string/split "a, b,c" #",\s*")          ;; => ["a" "b" "c"]
(clojure.string/replace "2026-10-24" #"-" "/")   ;; => "2026/10/24"
```

A grep over standard input, counting the slow errors and adding up their
latency, in one pass:

```clojure
(def slow-error #"ERROR .*latency=(\d{4,})ms")

(let [[n total] (reduce (fn [[n total] line]
                          (if-let [[_ ms] (re-find slow-error line)]
                            [(inc n) (+ total (parse-long ms))]
                            [n total]))
                        [0 0]
                        (line-seq *in*))]
  (println "matches" n "latency-ms-total" total))
```

`parse-long` and `parse-double` answer nil for a string that spells no
number, as in Clojure 1.11.

## 15.3 Namespaces in a script

A script can start with an `ns` form and require the libraries it uses
under their aliases, as Clojure code does:

```clojure
(ns my.report
  (:require [clojure.string :as str]
            [clojure.set :as set]
            [cheshire.core :as json]))

(println (str/upper-case "ok") (set/union #{1} #{2}) (json/generate-string [1 2]))
```

protoClojure has no full namespaces yet (D52): `ns` and `require` know the
library namespaces it provides (clojure.core, clojure.string, clojure.set,
clojure.java.io, clojure.java.shell, clojure.edn, cheshire.core,
clojure.data.json) and record their aliases; any other namespace is
`FileNotFoundException`. Every definition of the script is a global, and
the name in `ns` is ignored. The library functions also answer to their
full names without a `require` (`clojure.string/join`,
`cheshire.core/parse-string`).

## 15.4 JSON

Both JSON libraries Clojure scripts use are there, with their own names,
over one C++ parser and writer (LANGUAGE.md §19.2). cheshire:

```clojure
(require '[cheshire.core :as json])

(def order (json/parse-string "{\"id\":7,\"items\":[{\"sku\":\"a\",\"qty\":2}],\"note\":null}" true))
(:id order)                                   ;; => 7
(-> order :items first :qty)                  ;; => 2
(json/generate-string {:id 7 :tags ["x" "y"]}) ;; => "{\"id\":7,\"tags\":[\"x\",\"y\"]}" (keys in some order)
(json/generate-string [1 2] {:pretty true})   ;; => "[ 1, 2 ]"
```

and clojure.data.json:

```clojure
(require '[clojure.data.json :as dj])
(dj/read-str "{\"a\":[1,2.5]}" :key-fn keyword)   ;; => {:a [1 2.5]}
(dj/write-str {:path "a/b"})                       ;; => "{\"path\":\"a\\/b\"}"
```

Numbers without a fraction are integers of any size; others are doubles.
Keys are strings unless you ask for keywords (`true` in cheshire,
`:key-fn keyword` in data.json). A malformed document raises an exception
that says where: `JSON error: expected ':' after an object key, found
character '1' at line 1, column 6`.

A JSON transform, from a file named on the command line: keep the active
orders, group them by country, and print one row per country.

```clojure
(require '[cheshire.core :as json])

(spit "orders.json"
      "[{\"country\":\"AR\",\"active\":true,\"cents\":500},
        {\"country\":\"UY\",\"active\":true,\"cents\":250},
        {\"country\":\"AR\",\"active\":false,\"cents\":900},
        {\"country\":\"AR\",\"active\":true,\"cents\":100}]")

(def orders (json/parse-string (slurp (or (first *command-line-args*) "orders.json")) true))

(def rows (->> orders
               (filter :active)
               (group-by :country)
               (map (fn [[country rs]] [country (count rs) (reduce + (map :cents rs))]))
               (sort-by first)))

(println (json/generate-string (vec rows)))   ;; prints [["AR",2,600],["UY",1,250]]
```

The rows are arrays because the order of a map's keys is unspecified in
Clojure, and protoClojure's differs from the JVM's (D24): a script whose
output must not change between runtimes should not depend on it.

Newline-delimited JSON, one record per line, is read value by value with
`parse-stream`, which pulls its input a line at a time and answers nil at
the end:

<!-- not run: reads newline-delimited JSON on standard input (checked by hand: printf '{"qty":2}\n{"qty":5}\n' | protoclj ndjson.clj prints "records 2 qty 7") -->
```clojure
(require '[cheshire.core :as json])

(loop [n 0 qty 0]
  (if-let [rec (json/parse-stream *in* true)]
    (recur (inc n) (+ qty (:qty rec)))
    (println "records" n "qty" qty)))
```

## 15.5 EDN for configuration

EDN is Clojure's data notation. `clojure.edn/read-string` reads it with no
evaluation, which makes it the safe way to read a configuration file:

```clojure
(require '[clojure.edn :as edn])

(spit "config.edn" "{:port 8080 :hosts [\"a\" \"b\"] :debug? false}")
(def config (edn/read-string (slurp "config.edn")))
(:port config)                       ;; => 8080
(count (:hosts config))              ;; => 2
(= config (edn/read-string (pr-str config)))   ;; => true
```

`pr-str` writes what `read-string` reads back, so `spit` of `pr-str` and
`edn/read-string` of `slurp` save and restore any plain data.

## 15.6 Lazy sequences

`map`, `filter`, `range`, `iterate`, `for`, `line-seq` and their family are
lazy, as in Clojure: nothing is computed until something walks the
sequence, and an endless one is fine as long as you take only part of it.

```clojure
(take 5 (iterate #(* 2 %) 1))                         ;; => (1 2 4 8 16)
(take-while #(< % 30) (map #(* % %) (range)))        ;; => (0 1 4 9 16 25)
(for [x (range 10) :when (odd? x) :let [y (* x x)]] y) ;; => (1 9 25 49 81)
(first (filter #(> % 1000) (map #(* % %) (range)))) ;; => 1024
```

Word frequencies over standard input, the classic of the genre: every word
is a lazy sequence element, and only the counts are kept.

```clojure
(require '[clojure.string :as str])

(def freqs
  (->> (line-seq *in*)
       (mapcat #(re-seq #"[a-z']+" (str/lower-case %)))
       frequencies))

(doseq [[word n] (take 3 (sort-by (juxt (comp - val) key) freqs))]
  (println n word))
```

## 15.7 The same script under Babashka

`examples/showcase/` holds a grep, a JSON transform and a word count that
run unchanged under protoclj and `bb`, and `README.md` ("Showcase") shows
how long each takes and how much memory it needs under both. bb starts
faster, has far more libraries and is faster on these workloads. Where the
scripts have to be written differently it is because of these differences,
which are the ones a Babashka user is likely to meet:

- `*` promotes to an exact big integer instead of throwing on overflow
  (D14), so `(reduce * (range 1 1001))` works; `*'` works too, as in
  Clojure.
- `(/ 7 2)` is 3 (integer division); there are no ratios. Write
  `(quot 7 2)` to say so in both runtimes.
- There are no characters: `(first "abc")` is the string `"a"` (D3), so
  `(int \a)` has no protoClojure spelling; `(parse-long (str c))` turns a
  digit into its value in both.
- Map and set order is unspecified and differs from the JVM's (D24).
- No Java interop: `(System/getenv "HOME")`, `(System/exit 1)` and
  `System/currentTimeMillis` are there, `(.toUpperCase s)` is not; use
  `clojure.string`.
