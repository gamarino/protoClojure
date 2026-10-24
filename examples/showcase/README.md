# Showcase: the same scripts under protoclj and Babashka

Each script runs unchanged under `protoclj` and `bb`, prints a result that
checks the work it did, and is measured side by side by
`benchmarks/reports/2026-10-23-showcase/run.sh`. The numbers and what they
mean are in the main README, section "Showcase".

| Script | Reads | Prints |
|---|---|---|
| `grep.clj` | a log on standard input, line by line | the number of slow `ERROR` lines and the sum of their latencies |
| `json-transform.clj` | a JSON array of orders (the file named as the argument) | one JSON row per country: orders kept, total, average, top user |
| `word-freq.clj` | a text on standard input | the 20 most frequent words, the distinct and total counts |
| `factorial.clj` | nothing | the digits of `(factorial 1000)`, computed with `*'` |
| `factorial-star.clj` | nothing | the same with plain `*`: here the runtimes differ (bb overflows, protoclj promotes) |
| `empty.clj` | nothing | nothing (start-up time) |

Generate the inputs (about 190 MB; the same bytes on every machine) and run
a script:

```bash
python3 -I gen-data.py /tmp/showcase-data
protoclj grep.clj < /tmp/showcase-data/access.log
bb grep.clj < /tmp/showcase-data/access.log
protoclj json-transform.clj /tmp/showcase-data/orders.json
protoclj word-freq.clj < /tmp/showcase-data/corpus.txt
```

What the scripts avoid, so that they stay portable: map key order in the
output (the JSON rows are arrays), integer `/` (they use `quot`), and
characters (`(parse-long (str c))` instead of `(int c)`). The scripting
tutorial chapter (`docs/tutorial/15-scripting.md`) explains each.
