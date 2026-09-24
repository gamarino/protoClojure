# Installing protoClojure

protoClojure is a Clojure-flavoured language on the protoCore runtime. It is a
consumer of protoCore, never a bundler of it: `bin/protoclj` links
`libprotoCore.so.2`, and every package protoClojure produces declares a runtime
dependency on protoCore's own package instead of shipping a copy.

---

## Prerequisites

- A **C++20** compiler (GCC or Clang).
- **CMake** 3.20 or newer.
- **libreadline** (`libreadline-dev` on Debian/Ubuntu, `readline-devel` on
  Fedora/RHEL, `brew install readline` on macOS). It is a hard requirement: the
  REPL needs history, arrow-key editing and the multi-line continuation prompt,
  and configuration fails with a `FATAL_ERROR` when it is missing.
- **protoCore 2.1.0 or newer**, installed, with its CMake package
  configuration. See protoCore's `docs/INSTALLATION.md`.

---

## Building against an installed protoCore

```bash
# protoCore installed in a default prefix: nothing to pass.
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release

# protoCore installed elsewhere.
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DPROTO_CORE_PREFIX=$HOME/.local
# equivalently
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/.local

cmake --build build_release -j4
ctest --test-dir build_release --output-on-failure
```

The discovery is `find_package(protoCore 2.0 CONFIG)`, so the prefix must hold
`lib/cmake/protoCore/protoCoreConfig.cmake`. **A prefix holding only
`libprotoCore` and `protoCore.h` is no longer accepted**: without the package
configuration there is no way to tell protoCore 1.x from 2.x, and linking the
wrong major version is silent.

The version floor is `2.1` and the ceiling is the next major version.
protoClojure's maps are protoCore `ProtoMap`s and its actor mailboxes are
protoCore `ProtoMPSCQueue`s, both added in 2.1.0, and protoCore's major
version and its soname move together.
protoClojure additionally asserts that the package's `SOVERSION` is `2`.

## Building against a sibling developer tree

When no installed package is found *and* no prefix was named, protoClojure falls
back to the sibling source tree `../protoCore`, searching `build_release`, then
`build`, then `build_check`. The fallback prints a `WARNING`: it performs no
package version check (it does check that the build carries `SOVERSION 2`) and
must not be used to produce a distributable package.

Pass `-DPROTOCORE_REQUIRE_PACKAGE=ON` to turn the fallback into a hard error.
**Every packaging build sets it.** Switching a build directory between the two
modes leaves a stale `PROTOCORE_LIBRARY` cache entry; delete the build directory
rather than reconfiguring in place.

---

## Installing

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build_release -j4
cmake --install build_release --component protoClojure
```

Installed layout, relative to the prefix:

| Content | Location |
|---------|----------|
| `protoclj` | `bin/` |
| `LICENSE`, `README.md` and the Markdown documentation | `share/doc/protoClojure/` |
| Example `.clj` scripts | `share/protoClojure/examples/` |
| Benchmark scripts and their published results | `share/protoClojure/benchmarks/` |

**`protoclj` needs no external data file at run time.** The reader, the compiler
and the core namespaces are compiled into the binary; the examples and the
benchmarks are reference material a user can run, not a runtime dependency. The
only paths `protoclj` reads from the environment are `PROTOCLJ_ACTOR_WORKERS`
(the actor pool size) and `HOME` (the REPL history file).

protoClojure installs **no** copy of protoCore. `bin/protoclj` carries the
install RPATH `$ORIGIN/../<libdir>` (`@executable_path/../<libdir>` on macOS),
so a protoCore installed into the same prefix is found with no
`LD_LIBRARY_PATH`.

---

## Packages (CPack)

```bash
cmake -S . -B build_pkg -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<protocore-prefix> -DPROTOCORE_REQUIRE_PACKAGE=ON
cmake --build build_pkg -j4
cd build_pkg && cpack -G DEB
```

Generators are chosen at configure time; the DEB and RPM generators are enabled
only when `dpkg` and `rpmbuild` are found, because `cpack` aborts the whole run
when a generator's tool is missing and would take the TGZ down with it. Each
configure prints whether a generator was enabled or disabled, and why.

Package names are pinned rather than left to each generator's default casing:
`protoclojure` for DEB, `protoClojure` for RPM. Both declare a bounded
dependency on protoCore's own package:

| Format | Relation |
|--------|----------|
| DEB | `Depends: protocore (>= 2.1.0), protocore (<< 3.0.0)` |
| RPM | `Requires: protoCore >= 2.1.0, protoCore < 3.0.0` |

`libreadline` is a real runtime dependency of `protoclj` and is **not** declared
in the DEB; `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is not enabled for protoClojure.

### Platform verification status

| Platform | Packaging | Status |
|----------|-----------|--------|
| Linux | TGZ, DEB (needs `dpkg`), RPM (needs `rpmbuild`) | Built, installed to a scratch prefix and smoke-tested, including the extracted `.deb` payload |
| macOS | DragNDrop | Configured and reviewed, **never built** — no macOS host |
| Windows | NSIS, ZIP | Configured and reviewed, **never built** — no Windows host |

RPM packaging is configured and reviewed but **never executed**: `rpmbuild` is
not installed on the host this was verified on.
