#!/usr/bin/env python3
"""Regenerates or checks the bytecode examples of docs/VM.md.

    python3 -I docs/vm-examples/disasm_examples.py PROTOCLJ [--check | --update]

Every docs/vm-examples/NN-name.clj is run as `PROTOCLJ --disasm NN-name.clj`;
its listing is the golden file NN-name.txt next to it. docs/VM.md embeds each
example after the marker line `<!-- vm-example: NN-name -->` as a ```clojure
block (the source) followed by a ```text block (the listing).

--check (the default, and the ctest `docs/vm-examples`) fails when a listing
differs from its golden file, when VM.md's copy of a source or listing
differs from the files, or when an example is missing from VM.md. So a change
to the compiler that changes the bytecode cannot leave the documentation
behind silently.

--update rewrites the golden files and VM.md's copies from the current
protoclj. Review the diff: the prose around each example explains the
listing instruction by instruction.

Exit status 0 when everything matches (or after an update).
"""
import pathlib, re, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
VM_MD = HERE.parent / "VM.md"
MARKER = re.compile(r"^<!-- vm-example: (\S+) -->$")


def listing(protoclj, source):
    r = subprocess.run([protoclj, "--disasm", source.name], cwd=HERE,
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        sys.exit(f"FAIL {source.name}: protoclj exited {r.returncode}: {r.stderr.strip()}")
    return r.stdout.replace("\r\n", "\n")


def embed(md, name, source_text, listing_text):
    """VM.md with example `name` replaced; None when its marker is missing."""
    lines = md.split("\n")
    for i, line in enumerate(lines):
        m = MARKER.match(line)
        if not m or m.group(1) != name:
            continue
        # The marker is followed by a ```clojure block and a ```text block,
        # with only blank lines between them.
        j = i + 1
        blocks = []
        while len(blocks) < 2:
            while j < len(lines) and lines[j].strip() == "":
                j += 1
            if j >= len(lines) or not lines[j].startswith("```"):
                sys.exit(f"FAIL VM.md: example {name} must be followed by a ```clojure "
                         "and a ```text block")
            start = j
            j += 1
            while j < len(lines) and lines[j] != "```":
                j += 1
            blocks.append((start, j))
            j += 1
        new = (["```clojure"] + source_text.rstrip("\n").split("\n") + ["```", ""] +
               ["```text"] + listing_text.rstrip("\n").split("\n") + ["```"])
        return "\n".join(lines[:i + 1] + new + lines[blocks[1][1] + 1:])
    return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    protoclj = str(pathlib.Path(sys.argv[1]).resolve())
    update = "--update" in sys.argv[2:]
    md = VM_MD.read_text(encoding="utf-8").replace("\r\n", "\n")
    original_md = md
    failed = 0
    sources = sorted(HERE.glob("*.clj"))
    if not sources:
        sys.exit("FAIL: no examples in docs/vm-examples")
    for source in sources:
        name = source.stem
        source_text = source.read_text(encoding="utf-8").replace("\r\n", "\n")
        out = listing(protoclj, source)
        golden = source.with_suffix(".txt")
        if update:
            golden.write_text(out, encoding="utf-8", newline="\n")
        elif not golden.exists() or golden.read_text(encoding="utf-8").replace("\r\n", "\n") != out:
            failed += 1
            print(f"FAIL {name}: the listing differs from {golden.name}")
            print(out)
            continue
        new_md = embed(md, name, source_text, out)
        if new_md is None:
            failed += 1
            print(f"FAIL {name}: VM.md has no `<!-- vm-example: {name} -->` marker")
            continue
        if not update and new_md != md:
            failed += 1
            print(f"FAIL {name}: VM.md's copy differs from {source.name} / {golden.name}")
        md = new_md
        print(f"ok   {name}")
    if update and md != original_md:
        VM_MD.write_text(md, encoding="utf-8", newline="\n")
        print("updated docs/VM.md")
    if failed:
        print(f"{failed} example(s) out of date: run with --update and review the diff")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
