# protoClojure — embedder conformance

The normative rule table is `protoCore/docs/EMBEDDER-CONFORMANCE.md`.

- Static ratchet: `conformance-allow.txt` — **clean, exit 0**
- Runtime adaptor: **NOT WRITTEN IN P4.** See "What is not covered".
- End-to-end evidence for rule 2b: `tests/cli/blocking-joins-park-for-gc.sh`

## What is not covered

**protoClojure has no `proto::conformance::Host` yet**, so the twelve runtime
cases have not been run against it: rules 1, 3, 4, 5, 8, 9b, 9c and 11 are
**UNVERIFIED by this suite** — not passed. The obstacle is concrete and worth
recording rather than apologising for: protoClojure has **no compact
"construct a runtime and evaluate a string" API**. The whole wiring is inlined in
`runFile()` (`src/main.cpp:73-288`) and duplicated in the REPL's `Session`, and
`ExecutionEngine::run` takes 23 arguments. A `Host` needs a reusable factory
extracted from one of those two, which is a refactor with its own review, not an
adaptor.

Rules 2 and 2b **are** covered, and by better evidence than a conformance case:
see below.

## Rule 2b — the kernel fix, verified end to end here

protoClojure is where the `ProtoThread::join` deadlock was diagnosed, off a live
backtrace, and `tests/cli/blocking-joins-park-for-gc.sh` covers all four of its
blocking-join sites under a 500,000-cell heap ceiling, asserting each program's
computed value and checking its own premise that cycles ran and reclaimed while it
waited.

**The experiment the maintainer asked for, and its result:**

| Configuration | Result |
|---|---|
| protoClojure's four `UnmanagedScope` guards present, protoCore 2.3 (kernel brackets `join`) | 393/393; fixture passes |
| **All four guards removed**, protoCore 2.3 | **fixture passes in 8.3 s** |
| All four guards removed, protoCore `9e85a4c0` (no kernel fix) | **killed at 90 s, exit 124** |

So the kernel fix carries the fixture on its own. The guards were then
**restored**, and the reason is not about nesting: nesting is provably idempotent
(`unmanagedDepth` is a counter; only the outermost pair moves `parkedThreads`), so
removing them would have been safe on the code in front of me. They are kept
because protoClojure's build **cannot detect the fix**: it checks `SOVERSION`,
which is 3 both before and after, because the fix changed no ABI. A protoClojure
linked against protoCore 2.2.0 with the guards removed would deadlock with no
warning. Each guard now carries that reasoning inline, and says to remove all four
once every embedder requires protoCore 2.3.

The fixture's own header premise — *"`ProtoThread::join` is a bare
`std::thread::join`"* — was true when it was written and is now false. It has been
updated rather than deleted: the fixture is now a check on the **kernel** as much
as on protoClojure, and it is the only end-to-end evidence in the family that the
kernel guard works under real GC pressure.

## Static check — 2026-09-25

**0 unjustified findings**, 2 justified errors, 4 justified warnings, 2
informational.

Both `blocking_join_unbracketed` errors are joins protoCore cannot see, and both
are correct **only by construction**, which is why each carries the condition
under which that stops being true:

- `src/runtime/GCCensus.cpp:59` joins the `PROTOCLJ_GC_STATS` sampler, a raw
  `std::thread` that touches no `ProtoObject*` and so is not in `runningThreads`.
  The registered main thread therefore holds the quorum for at most the ~200 µs
  the sampler needs to see `stop_`.
- `src/runtime/StackGuard.cpp:146` is `main()`'s thread waiting for the evaluator
  thread. The `ProtoSpace` is constructed **inside** the joined thread, so at the
  moment of the join the joining thread is registered with no space.

## Judgement items

**C3, C5 and C7 are unanswered** and belong with the `Host` work. Two of the three
already have most of an answer in the tree:

- **C5** — protoClojure's vector is **not** a `ProtoTuple`: Track C moved it to a
  `ProtoList` in a one-entry sparse-list box precisely because *"protoCore interns
  every tuple node and never frees one"* (`src/runtime/VectorOps.h:24-34`), and two
  unit tests pin it. The three textual `ProtoTuple` hits in `src/` are all
  comments saying not to use one.
- **C7** — **N/A in one line: protoClojure has zero external wrappers.**
  `fromExternalPointer` and `newExternalBuffer` do not appear in `src/` at all, so
  `externalBytesAccounted()` would correctly return −1.
- **C3** is the open one, and it has history: `ActorMessage` held raw
  `ProtoObject*` payloads in a lock-free structure and was deleted for it, with
  messages becoming a `ProtoList` in a rooted `ProtoMPSCQueue`. Whether any other
  path holds a pointer across an allocation in a bare local is what
  `gc.host_stress` under ASan would answer, and it has not been run.

## Informational

- protoClojure never calls `setHeapLimits` and installs no
  `outOfMemoryCallback`, so no cycle starts by itself in an ordinary run and rule
  8 is unreachable as configured. Its GC-observing CLI fixtures set
  `PROTOCORE_HEAP_LIMIT_CELLS` from outside for exactly this reason.
- The safepoint added on a 64-event stride in the VM
  (`src/runtime/ExecutionEngine.h:184-186`, `.cpp:242-256`) has a documented
  kill switch, `PROTOCLJ_NO_GC_SAFEPOINT=1`, and
  `tests/cli/loop-garbage-is-reclaimed.sh` already runs the A/B **in both
  directions** — it fails if the disabled run succeeds. That is the shape a
  rule-1 conformance case should take when the `Host` is written.
