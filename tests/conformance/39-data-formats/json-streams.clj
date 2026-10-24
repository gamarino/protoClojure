;; EXPECT: [{:id 1} true nil [1 2] {k v} :eof [3]]
;; parse-stream reads one value from a reader, a line at a time, so a file of
;; newline-delimited JSON is read value by value, and nil at the end;
;; generate-stream and clojure.data.json/write write to a writer;
;; clojure.data.json/read reads from a reader, :eof-error? false answering
;; :eof-value at the end.
(require '[cheshire.core :as json] '[clojure.data.json :as dj])
(def dir (trim (:out (sh "sh" "-c" "d=$(mktemp -d); if command -v cygpath >/dev/null 2>&1; then cygpath -m \"$d\"; else echo \"$d\"; fi"))))
(spit (str dir "/in.ndjson") "{\"id\":1}\n{\"id\":2,\n \"tags\":[\"a\",\"b\"]}\n")
(def from-stream
  (with-open [r (reader (str dir "/in.ndjson"))]
    [(json/parse-stream r true)
     (= {:id 2 :tags ["a" "b"]} (json/parse-stream r true))
     (json/parse-stream r true)]))
(with-open [w (writer (str dir "/out.json"))] (json/generate-stream [1 2] w))
(with-open [w (writer (str dir "/out2.json"))] (dj/write {"k" "v"} w))
(spit (str dir "/three.json") "[3]")
(def from-data-json
  (with-open [r (reader (str dir "/three.json"))]
    [(dj/read r) (dj/read r :eof-error? false :eof-value :eof)]))
(def result (vec (concat from-stream
                         [(json/parse-string (slurp (str dir "/out.json")))
                          (dj/read-str (slurp (str dir "/out2.json")))]
                         (reverse from-data-json))))
(sh "rm" "-rf" dir)
(println result)
