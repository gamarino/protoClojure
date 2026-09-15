# 13. Actors

Atoms (chapter 6) give you a shared cell that any thread can read and
swap. Futures (chapter 6.7) give you a parallel computation that
materialises a value later. Both are useful, neither is what you reach
for when you need a stateful thing that *processes messages one at a
time*.

That last one is an **actor**: a tiny entity that owns a value, accepts
function messages from any thread, and runs them — strictly one at a
time — on a worker drawn from a shared pool. The body of each message
is just a Clojure function, the value-so-far is its first argument, the
return value is the new value-so-far. No locks in your code.

This chapter walks through the surface, the guarantees, the priority
bands, and the worker-pool sizing.

## 13.1 The basic moves

```clojure
(def acc (actor 0))           ;; create an actor holding 0
@acc                          ;; => 0  — current value, no message sent

(send acc inc)                ;; => promise — enqueued at medium priority
(send acc + 100)              ;; => promise — extra args after the function
@(send acc inc)               ;; => 102 — block until that message is processed
@acc                          ;; => 102 — the value-so-far
```

`send` always returns immediately with a promise that will hold the
message's result. The actor's value is updated in-place by the worker;
you can also read it via `@actor` without sending a message — that read
sees whatever value the most recently completed message left behind,
with no ordering guarantee against still-pending messages.

`actor?` distinguishes actors from anything else:

```clojure
(actor? acc)                  ;; => true
(actor? (atom 0))             ;; => false
```

The print form is deliberately distinct from atoms, so you see what you
have at the REPL:

```
#<actor 102>
```

## 13.2 The single-method invariant

This is the load-bearing property and the reason actors exist at all:

> **At any instant, at most one message per actor is being processed.**

That is enforced by the scheduler, not by your code. The function body
of a `send`'d message sees a coherent value — no other worker can touch
this actor while you are inside. So you write:

```clojure
(defn apply-op [account msg]
  (cond
    (= (get msg :op) :deposit)  (assoc account :balance (+ (get account :balance) (get msg :amount)))
    (= (get msg :op) :withdraw) (assoc account :balance (- (get account :balance) (get msg :amount)))
    :else account))

(def bank-account (actor {:balance 0}))
(send bank-account apply-op {:op :deposit :amount 50})
@(send bank-account apply-op {:op :withdraw :amount 20})   ;; => {:balance 30}
```

with no `swap!`, no CAS retry loop, no `locking`. The invariant covers
the body for free.

What it does *not* give you:

- Ordering across actors. Two sends to two different actors can run in
  parallel and finish in any order.
- Total ordering of unrelated senders. If thread A and thread B both
  `send` to the same actor, the actor processes them in mailbox-arrival
  order — but which arrived first depends on scheduler timing.
- Cross-actor atomicity. There is no transaction spanning two actors.

In return: the entire body of the message function is single-threaded
relative to that actor. No locks. No retries. No ABA. That is the deal.

## 13.3 Priority bands

Every actor system that runs long enough wants priority. protoClojure's
scheduler keeps three ready queues, drained in strict order:

```
high → medium → low
```

A worker picking up the next ready actor always drains the **highest
non-empty queue first**. Within a queue, FIFO.

```clojure
(send-h acc inc)              ;; HIGH:   urgent
(send   acc inc)              ;; MEDIUM: default — same as plain (send …)
(send-l acc inc)              ;; LOW:    background
```

The priority attaches to the **enqueue**, not to the actor itself. A
single actor can have a high-priority message and a low-priority message
in flight at the same time; the scheduler runs the high one first.

The priority does **not** override the single-method invariant: if a
low-priority message is in the middle of running, a newly-arrived
high-priority message waits until that low one finishes. Priority
controls scheduler choice, not preemption.

A good rule of thumb:

- `send-h` for control-plane messages (shutdown, drain, reconfigure).
- `send` for the bread-and-butter data-plane.
- `send-l` for hygiene work that can wait (telemetry, GC hints, log
  flushes).

## 13.4 The worker pool

There is one pool of workers per process, shared by all actors. Workers
are real OS threads — created via protoCore's `newThread`, so they
participate in the GC quorum like every other thread.

The pool size is read once from the environment at startup:

```bash
PROTOCLJ_ACTOR_WORKERS=4  ./protoclj script.clj
```

If the variable is unset (or invalid), the default is
`max(2, hardware_concurrency() − 2)`, capped at 16. Two workers
is the floor; 16 is the ceiling.

Rule of thumb for sizing:

- CPU-bound message bodies → set it to the number of physical cores.
  Hyperthreads usually regress.
- I/O-bound bodies (`future`-ing a slow upstream from inside the actor)
  → can profitably go higher than core count.
- Mostly idle, just a few actors with bursty load → the default is fine.

Read the running stats with `actor-stats`:

```clojure
(actor-stats)
;; => {:messages-processed 100000, :workers 4}
```

## 13.5 Wiring atoms, futures, and actors together

The three primitives compose. A common pattern: an actor that owns a
piece of state, a future that does the slow work, and atoms for
lock-free counters around them.

```clojure
(def hits (atom 0))                       ;; lock-free counter
(def cache (actor {}))                    ;; owned by the actor

(defn ensure [k]
  (swap! hits inc)
  ;; If the key isn't there, fetch it on a future, store via send
  (let [cur @cache]
    (if (contains? cur k)
      (get cur k)
      (let [f (future (slow-fetch k))]
        (send cache assoc k @f)
        @f))))
```

The actor's mailbox serialises the `assoc` calls; the futures parallelise
the slow fetches; the atom counts requests. Each primitive does the one
thing it is good at.

## 13.6 What `actor` is not

It is **not** Clojure-on-the-JVM's `agent`. Some surface looks similar
on purpose, but:

- `actor` always returns a promise from `send`. Clojure's `agent` does
  not — `(send agent f)` returns the agent itself.
- `actor` has three priority bands. `agent` has one.
- `actor` does not (yet) have an error-mode setting, `restart-agent`,
  `await`, or `await-for`.
- The single-method invariant is the same in both.

The JVM `agent` surface is planned but not implemented — see
[`STATUS.md`](../STATUS.md). The current `actor` is the
protoCore-native shape: closer to protoST's actor, simpler than Akka,
deliberately small.

## 13.7 Honest numbers

On a 2026-06-14 measurement (AMD Ryzen 5 5500U, 6 cores, 12 threads),
running 1,000,000 messages per row, body = `(inc v)`:

| mode      | what it measures                                              | peak msg/s |
|-----------|---------------------------------------------------------------|-----------:|
| `single`  | 1 sender × 1 actor                                            |   215,100  |
| `fan-out` | 1 sender × 1000 actors                                        |   218,341  |
| `MPSC`    | 4 sender threads × 1 actor (multi-producer single-consumer)   |   171,851  |
| `MPMC`    | 4 sender threads × 4 actors round-robin                       |   125,424  |

A few things to read out of those numbers:

- **`single` and `fan-out` are essentially equal.** The per-actor
  pipeline (send → mailbox → process → promise) costs the same whether
  you keep hitting one actor or spread across thousands. The scheduler
  overhead is not a separate visible cost in this regime.
- **`MPSC` is slower than `single`** despite having more sender threads,
  because the single actor processes serially and now also pays the
  cost of cross-thread coordination on the lock-free mailbox.
- **`MPMC` is slower again** because the workers also contend on a
  global ready-queue mutex. That's the next optimisation target.
- **Worker count scaling** peaks at 6 (the physical-core count of this
  CPU) for `fan-out`, and falls off at 8/16 because the extra workers
  go to SMT siblings — same pattern protoST documented.

The benchmark sources are `benchmarks/actor-throughput.clj`,
`actor-fanout.clj`, `actor-mpsc.clj` and `actor-mpmc.clj`; the runner is
`benchmarks/actor-bench.sh`. Treat these as **upper bounds for a
single-operation body**: message bodies that do real work lower them.

> Note: earlier versions of this document quoted "~5M msg/s single" and
> "~250K msg/s fan-out". Those numbers were wrong — the benchmark script
> was silently failing to compile and the runner was measuring
> time-to-failure instead of throughput. The runner now checks each
> script's `:messages-processed` output before reporting a rate.

## 13.8 Summary

- `(actor v)` — make an actor holding `v`.
- `(send a f & args)` — enqueue `(f current & args)` at medium priority;
  returns a promise of the result.
- `(send-h …)` / `(send-l …)` — same shape, high / low priority bands.
- `(actor? x)` — predicate.
- `(actor-stats)` — `{:workers N :messages-processed K}`.
- `@actor` — current value, no message round-trip.
- Single-method invariant guarantees no concurrent body execution per
  actor; you do not need locks inside the function.
- Worker pool sized by `PROTOCLJ_ACTOR_WORKERS` env var
  (default `max(2, cores − 2)`, cap 16).
