#!/usr/bin/env python3
"""Generates the showcase inputs, deterministically (a fixed seed).

    python3 gen-data.py OUTDIR [--log-mb 110] [--orders 300000] [--corpus-mb 30]

  OUTDIR/access.log   web-server log lines, about LOG_MB megabytes
  OUTDIR/orders.json  one JSON array of ORDERS order records
  OUTDIR/corpus.txt   English-like text with a Zipf word distribution

The same seed gives the same bytes on every machine (Python's Mersenne
Twister), so both runtimes always read identical input.
"""
import argparse, itertools, json, os, random

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--log-mb", type=int, default=110)
    ap.add_argument("--orders", type=int, default=300000)
    ap.add_argument("--corpus-mb", type=int, default=30)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    rng = random.Random(20261024)

    # access.log: ISO time, level, user, path, status, latency.
    levels = ["INFO"] * 90 + ["WARN"] * 7 + ["ERROR"] * 3
    paths = ["/api/orders", "/api/users", "/login", "/static/app.js", "/api/search?q=clojure",
             "/health", "/api/orders/42/items", "/logout"]
    statuses = [200] * 80 + [201, 204, 301, 304, 400, 401, 403, 404, 500, 502, 503] * 2
    limit = a.log_mb * 1024 * 1024
    with open(os.path.join(a.outdir, "access.log"), "w", encoding="utf-8", newline="\n") as f:
        size = 0
        i = 0
        while size < limit:
            i += 1
            lvl = rng.choice(levels)
            line = ("2026-10-%02dT%02d:%02d:%02d %s user=u%05d path=%s status=%d latency=%dms\n" %
                    (1 + i % 28, i % 24, i % 60, (i * 7) % 60, lvl, rng.randrange(100000),
                     rng.choice(paths), rng.choice(statuses), int(rng.expovariate(1 / 300.0))))
            f.write(line)
            size += len(line)

    # orders.json: an array of records.
    countries = ["AR", "BR", "CL", "DE", "ES", "FR", "IT", "JP", "MX", "US", "UY", "ZA"]
    names = ["Zoë", "José", "Ana", "Björn", "Chen", "Ngozi", "Ola", "Mía", "Kenji", "Lars"]
    recs = []
    for i in range(1, a.orders + 1):
        recs.append({
            "id": i,
            "user": "%s-%d" % (rng.choice(names), rng.randrange(5000)),
            "country": rng.choice(countries),
            "active": rng.random() < 0.7,
            "score": rng.randrange(101),
            "amount_cents": rng.randrange(100, 500000),
            "tags": rng.sample(["new", "gift", "express", "bulk", "promo", "retry"], rng.randrange(4)),
            "note": None if rng.random() < 0.5 else "line \"%d\"\n\ttab \\ slash /" % i,
        })
    with open(os.path.join(a.outdir, "orders.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(recs, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")

    # corpus.txt: sentences over a vocabulary drawn with a Zipf-like weight.
    syll = ["ka", "lo", "mi", "ra", "te", "su", "no", "vi", "da", "pe", "ri", "go", "ba", "ze", "lu"]
    vocab = []
    seen = set()
    while len(vocab) < 20000:
        w = "".join(rng.choice(syll) for _ in range(rng.randrange(1, 5)))
        if rng.random() < 0.05:
            w += "'s"
        if w not in seen:
            seen.add(w)
            vocab.append(w)
    weights = [1.0 / (r + 1) for r in range(len(vocab))]
    cum = list(itertools.accumulate(weights))  # what choices() computes from weights
    limit = a.corpus_mb * 1024 * 1024
    with open(os.path.join(a.outdir, "corpus.txt"), "w", encoding="utf-8", newline="\n") as f:
        size = 0
        while size < limit:
            words = rng.choices(vocab, cum_weights=cum, k=rng.randrange(6, 16))
            words[0] = words[0].capitalize()
            line = " ".join(words) + rng.choice([".", ",", "!", "?", ";"]) + "\n"
            f.write(line)
            size += len(line)

if __name__ == "__main__":
    main()
