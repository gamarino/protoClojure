#!/usr/bin/env bash
#
# CLI check: an actor message's payload is rooted while it waits in a mailbox.
#
# Until the mailboxes became protoCore `ProtoMPSCQueue`s, a queued send lived
# in a `std::atomic<ActorMessage*>` stack of C++ heap nodes, and the function,
# the arguments and the promise those nodes held were **not** GC roots. A
# collection between the send and the handler could therefore free a payload
# that nothing else referenced. The three queues now hang off the actor's
# wrapper object, which is mutable and therefore a root, so the collector
# traces every queued message.
#
# This is a fixture with a premise. It sends messages whose arguments are
# freshly built collections referenced by nothing but the mailbox, under a
# heap ceiling low enough that collections run throughout the sends, and each
# handler checks its own payload for self-consistency. Removing the
# `__mailbox__` attribute from the wrapper in `prim_actor` — the one line that
# roots the queues — makes it fail: verified, it aborts inside the collector.
#
# The program is generated on the fly and read from standard input, so the
# check writes no files. Registered as the `cli/actor-payloads-survive-gc`
# ctest case by tests/CMakeLists.txt.
#
# Usage: actor-payloads-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: actor-payloads-survive-gc.sh <protoclj>}"
ROUNDS=50
PER_ROUND=100
CHURN=3000
MSGS=$((ROUNDS * PER_ROUND))
LIMIT=400000
WORKERS=4

program() {
    cat <<CLJ
;; Every message carries a fresh vector [i "payload-<i>" (i i)] that only the
;; mailbox references once the sending iteration has moved on. The handler
;; adds 1 only when its payload is still internally consistent, so a freed or
;; overwritten payload shows up as a count below $MSGS rather than as a pass.
(defn check [state v]
  (if (and (= (count v) 3)
           (= (nth v 1) (str "payload-" (nth v 0)))
           (= (nth v 2) (list (nth v 0) (nth v 0))))
    (+ state 1)
    state))
(def a (actor 0))
;; One burst of sends, and one round of throwaway allocation. Both run in
;; their own call frame, so protoCore is shown their garbage when the frame
;; returns — which is what lets the heap ceiling force collection cycles
;; while messages are still queued. Garbage left inside ONE frame could not be
;; collected at all, and the run would abort instead.
(defn burst [base n]
  (loop [i 0 f nil]
    (if (>= i n)
      f
      (recur (+ i 1)
             (send a check (vector (+ base i)
                                   (str "payload-" (+ base i))
                                   (list (+ base i) (+ base i))))))))
(defn churn [k]
  (loop [i 0 acc nil]
    (if (>= i k) acc (recur (+ i 1) (str "collectable-garbage-" i)))))
(defn rounds [r base f]
  (if (= r 0)
    f
    (let [last-f (burst base $PER_ROUND)]
      (churn $CHURN)
      (rounds (- r 1) (+ base $PER_ROUND) last-f))))
@(rounds $ROUNDS 0 nil)
(println @a)
CLJ
}

out=$(program | PROTOCLJ_ACTOR_WORKERS=$WORKERS \
      PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT "$PROTOCLJ" /dev/stdin 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
last=$(printf '%s\n' "$out" | awk 'NF { line = $0 } END { print line }')
if [[ "$last" != "$MSGS" ]]; then
    echo "FAIL: expected $MSGS intact payloads, got: $last"
    printf '%s\n' "$out" | tail -3
    exit 1
fi
echo OK
