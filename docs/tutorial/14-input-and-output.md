# 14. Input and output

A language becomes useful the moment it can read a file, run another
program and talk to the network. This chapter shows protoClojure doing all
three: files and standard input, `sh`, the environment, an HTTP client and
server, and plain sockets.

If you know Clojure, most names will be familiar: `slurp`, `spit`,
`with-open`, `line-seq` from `clojure.core`; `reader`, `writer`, `file`,
`copy`, `delete-file` from `clojure.java.io`; `sh` from
`clojure.java.shell`; the HTTP client has babashka's `http-client` shape
and the server follows Ring. There are no namespaces yet, so they are all
plain globals: you write `(sh "ls")`, not `(shell/sh "ls")`. Where Clojure
would reach for Java interop (`.close`, `System/getenv`), protoClojure has a
small function instead. `docs/LANGUAGE.md` §18 is the reference; the
differences from Clojure are deviations D28 to D33 in `docs/STATUS.md`.

Every example below is a complete script: save it as `demo.clj` and run
`protoclj demo.clj`. Remember that `let` and `loop` work only inside
function bodies in 0.0.1, so the examples define small functions.

## 14.1 Files

`spit` writes a string to a file, `slurp` reads it back:

```clojure
(spit "notes.txt" "first line\n")
(spit "notes.txt" "second line\n" :append true)
(println (slurp "notes.txt"))
;; first line
;; second line
```

`spit` writes `(str x)`, so `(spit "v.edn" [1 2 3])` stores `[1 2 3]`.

To read line by line, open a reader. `with-open` closes it when the body
ends, also when the body throws:

```clojure
(defn count-lines [path]
  (with-open [r (reader path)]
    (count (line-seq r))))

(spit "notes.txt" "a\nb\nc\n")
(println (count-lines "notes.txt"))   ;; 3
```

`line-seq` reads to the end of the reader and answers a list. In Clojure it
is lazy; here it is eager, because protoClojure has no lazy sequences yet
(D29). For a stream that never ends, read one line at a time with
`read-line`, which answers nil at the end.

Writers work the same way; `write` stands in for Java's `.write`:

```clojure
(with-open [w (writer "out.txt")]
  (write w "hello ")
  (write w 42))
(println (slurp "out.txt"))   ;; hello 42
```

`file` joins paths, and the `clojure.java.io` helpers take a path string
or a file:

```clojure
(def f (file "tmp-demo" "sub" "data.txt"))
(make-parents f)                       ;; creates tmp-demo/sub
(spit f "x")
(println (str f) (exists? f))          ;; tmp-demo/sub/data.txt true
(println (map str (file-seq (file "tmp-demo"))))
;; (tmp-demo tmp-demo/sub tmp-demo/sub/data.txt)
(delete-file f)
(delete-file (file "tmp-demo" "sub"))
(delete-file "tmp-demo")
```

## 14.2 When things go wrong

A failed operation throws an exception of the class you would catch in
Clojure, carrying a data map that says what happened:

```clojure
(println
  (try (slurp "no-such-file.txt")
       (catch FileNotFoundException e (ex-data e))))
;; {:type :file-not-found, :errno 2}
```

Every I/O exception is an `IOException` (except a refused argument, which
is an `IllegalArgumentException`), so one `catch` can cover them all:

```clojure
(defn safe-slurp [path]
  (try (slurp path)
       (catch IOException e
         (println "could not read" path "-" (:type (ex-data e)))
         "")))
(safe-slurp "/nonexistent/x")
;; could not read /nonexistent/x - :file-not-found
```

## 14.3 Standard input and a filter

`(read-line)` with no argument reads standard input. A classic Unix filter
— upper-case every line, then report how many there were:

```clojure
(defn pump [n]
  (let [line (read-line)]
    (if (nil? line)
      n
      (do (println (upper-case line))
          (recur (+ n 1))))))
(println "lines:" (pump 0))
```

```bash
$ printf 'one\ntwo\n' | protoclj upper.clj
ONE
TWO
lines: 2
```

The arguments after the script's path are in `*command-line-args*`, a list
of strings (nil when there are none), and `exit` ends the program with a
status:

```clojure
(when (nil? *command-line-args*)
  (println "usage: protoclj greet.clj NAME")
  (exit 2))
(println "hello," (first *command-line-args*))
```

```bash
$ protoclj greet.clj Ada
hello, Ada
```

## 14.4 Other programs

`sh` runs a program and answers a map with its exit status and both
outputs. A non-zero exit is an answer, not an exception:

```clojure
(def listing (sh "ls" "/"))
;; listing is {:exit 0, :out "bin\nboot\n...", :err ""}
(println (:exit listing) (first (split (:out listing) "\n")))   ;; 0 bin

(def r (sh "sh" "-c" "echo oops >&2; exit 3"))
(println (:exit r) (:err r))           ;; 3 oops
```

`:in` feeds the program's standard input, `:dir` sets its working
directory and `:env` its environment:

```clojure
(println (:out (sh "tr" "a-z" "A-Z" :in "shout\n")))   ;; SHOUT
(println (:out (sh "pwd" :dir "/tmp")))                ;; /tmp
(println (getenv "HOME"))                              ;; your home directory
```

## 14.5 An HTTP server and client

A Ring handler is a function from a request map to a response map. Start a
server with `run-server`, call it with `http-get`, stop it with
`stop-server`. `:port 0` picks a free port, which `server-port` tells you:

```clojure
(defn handler [req]
  (cond
    (= (:uri req) "/hello")
      {:status 200
       :headers {"content-type" "text/plain"}
       :body (str "hello, " (or (:query-string req) "world"))}
    (= (:request-method req) :post)
      {:status 201 :body (str "got " (count (:body req)) " characters")}
    :else
      {:status 404 :body "not found"}))

(def server (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port server)))

(println (:body (http-get (str base "/hello?ada"))))           ;; hello, ada
(println (:status (http-post (str base "/items")
                             {:headers {"content-type" "application/json"}
                              :body "{\"name\": \"ñandú\"}"})))  ;; 201
(println (:status (http-get (str base "/nope") {:throw false}))) ;; 404
(stop-server server)
```

As in babashka, a 404 would have thrown an `ExceptionInfo` carrying the
response in its `ex-data`; `:throw false` answers it instead.

Each connection is served on a thread of its own, so a slow handler never
holds up the others, and handlers may use atoms, futures and actors freely.
The server refuses malformed or oversized requests itself (400, 414, 431,
413), and a handler that throws produces a plain `500` and a line on
standard error. A server still running when your script ends is stopped
then; to keep serving, block the main thread, for example on
`@(promise)`.

`slurp` also fetches `http://` and `https://` URLs:

```clojure
(println (count (slurp "https://example.com/")))
```

## 14.6 Sockets

When HTTP is too much, speak TCP directly. The socket functions are
protoClojure's own (D30). An echo server on a future and a client:

```clojure
(def listener (tcp-listen "127.0.0.1" 0))

(def echo
  (future
    (let [conn (tcp-accept listener 5000)
          line (socket-read-line conn)]
      (socket-write conn (str "echo: " line "\n"))
      (socket-close conn))))

(defn ask [question]
  (with-open [s (tcp-connect "127.0.0.1" (server-port listener) {:timeout 5000})]
    (socket-write s (str question "\n"))
    (socket-read-line s)))

(println (ask "ping"))   ;; echo: ping
@echo
(close listener)
```

`{:timeout ms}` bounds the connect and every later wait on the socket, and
`{:tls true}` speaks TLS, verifying the server's certificate. UDP is
`udp-socket`, `udp-send` and `udp-receive`:

```clojure
(def a (udp-socket "127.0.0.1" 0))
(def b (udp-socket "127.0.0.1" 0))
(udp-send a "127.0.0.1" (server-port b) "hi")
(def d (udp-receive b 1000))     ;; {:data "hi", :host "127.0.0.1", :port ...}
(println (:data d) "from" (:host d))   ;; hi from 127.0.0.1
(close a)
(close b)
```

## 14.7 Summary

- Files: `slurp`, `spit` (`:append`), `reader`, `writer`, `with-open`,
  `read-line`, `write`, `close`, `line-seq` (eager), `file`, `file-seq`,
  `exists?`, `directory?`, `delete-file`, `make-parents`, `copy`.
- Programs: `sh` (`:in`, `:dir`, `:env`) → `{:exit :out :err}`; `getenv`,
  `exit`, `*command-line-args*`.
- HTTP: `http-get` / `http-post` / `http-put` / `http-delete` /
  `http-head` / `http-request` → `{:status :headers :body}`;
  `run-server`, `stop-server`, `server-port` with Ring maps.
- Sockets: `tcp-connect` (`:tls`, `:timeout`), `tcp-listen`,
  `tcp-accept`, `socket-read-line`, `socket-write`, `socket-close`,
  `udp-socket`, `udp-send`, `udp-receive`.
- Failures are `IOException`s (`FileNotFoundException`,
  `ConnectException`, `SocketTimeoutException`, ...) with
  `{:type ... :errno ...}` as their `ex-data`.
- Blocking calls never hold up the garbage collector; the HTTP server never
  uses the actor pool.
