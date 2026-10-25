#!/usr/bin/env python3
"""Runs the Clojure snippets of the documentation through protoclj.

    python3 -I tests/docs/check_snippets.py PROTOCLJ [FILE.md ...]

Every fenced ```clojure block of the given files (default: README.md,
docs/LANGUAGE.md, docs/TUTORIAL.md, docs/VM.md and docs/tutorial/*.md) is run as a
script, in a fresh temporary directory, with a small text on standard input.
A block passes when protoclj exits 0. Blocks are skipped when they are REPL
transcripts (`user=>`), or when the line just before the fence is the HTML
comment `<!-- not run: reason -->` (planned features, fragments, programs
that need a network or run forever), or `<!-- not run on Windows: reason -->`
on Windows only (POSIX paths and commands), or when one of the first lines of the
file is `<!-- snippets: not run (reason) -->` (a chapter about a planned
feature); every skip is listed with its reason. A block that fetches an
http(s) URL and fails because the server answered 5xx (example.com's
occasional 503) is skipped as a transient network failure, with the server's
answer as the reason: the server's state is not protoclj's.

On Windows the POSIX tools some snippets run (`tr`, `ls`) come from Git for
Windows: tests/CMakeLists.txt appends its usr/bin to PATH for this test.

Exit status 0 when every block that is run passes.
"""
import os, pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SAMPLE = "the quick brown fox\njumps over the lazy dog\nERROR latency=1200ms\nthe end\n"

def blocks(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    whole = next((re.match(r"<!--\s*snippets:\s*not run\s*\((.*?)\)\s*-->", l).group(1)
                  for l in lines[:5] if re.match(r"<!--\s*snippets:\s*not run", l)), None)
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)```clojure\s*$", lines[i])
        if not m:
            i += 1
            continue
        indent = m.group(1)
        prev = lines[i - 1].strip() if i > 0 else ""
        start = i + 1
        j = start
        while j < len(lines) and not re.match(r"^\s*```\s*$", lines[j]):
            j += 1
        body = "\n".join(l[len(indent):] if l.startswith(indent) else l for l in lines[start:j])
        skip = whole
        n = re.match(r"<!--\s*not run:\s*(.*?)\s*-->", prev)
        w = re.match(r"<!--\s*not run on Windows:\s*(.*?)\s*-->", prev)
        if n:
            skip = n.group(1)
        elif w and os.name == "nt":
            skip = "Windows: " + w.group(1)
        elif skip is None and "user=>" in body:
            skip = "REPL transcript"
        yield start + 1, body, skip
        i = j + 1

def main():
    protoclj = sys.argv[1]
    files = [pathlib.Path(f) for f in sys.argv[2:]] or (
        [ROOT / "README.md", ROOT / "docs/LANGUAGE.md", ROOT / "docs/TUTORIAL.md",
         ROOT / "docs/VM.md"]
        + sorted((ROOT / "docs/tutorial").glob("*.md")))
    ran = failed = skipped = 0
    for f in files:
        for line, body, skip in blocks(f):
            where = f"{f.relative_to(ROOT) if f.is_absolute() else f}:{line}"
            if skip:
                skipped += 1
                print(f"SKIP {where}: {skip}")
                continue
            ran += 1
            with tempfile.TemporaryDirectory() as d:
                p = pathlib.Path(d) / "snippet.clj"
                p.write_text(body + "\n", encoding="utf-8")
                try:
                    r = subprocess.run([protoclj, str(p)], input=SAMPLE, capture_output=True,
                                       text=True, cwd=d, timeout=30)
                    ok, err = r.returncode == 0, (r.stderr or r.stdout).strip().splitlines()[-1:] 
                except subprocess.TimeoutExpired:
                    ok, err = False, ["timeout"]
            if (not ok and re.search(r"https?://", body) and err
                    and re.search(r"HTTP response code: 5\d\d", err[0])):
                skipped += 1
                print(f"SKIP {where}: transient network failure, the server answered: {err[0]}")
                continue
            if not ok:
                failed += 1
                print(f"FAIL {where}: {err[0] if err else ''}")
    print(f"{ran} run, {failed} failed, {skipped} skipped")
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
